/*
 * SGI FREE SOFTWARE LICENSE B (Version 2.0, Sept. 18, 2008)
 * Copyright (C) 1991-2000 Silicon Graphics, Inc. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice including the dates of first publication and
 * either this permission notice or a reference to
 * http://oss.sgi.com/projects/FreeB/
 * shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * SILICON GRAPHICS, INC. BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Except as contained in this notice, the name of Silicon Graphics, Inc.
 * shall not be used in advertising or otherwise to promote the sale, use or
 * other dealings in this Software without prior written authorization from
 * Silicon Graphics, Inc.
 */
/*
** Author: Eric Veach, July 1994.
**
*/

#include "gluos.hpp"
#include <fastuidraw/util/util.hpp>
#include <stddef.h>
#include <setjmp.h>             /* longjmp */
#include <limits.h>             /* LONG_MAX */

#include "mesh.hpp"
#include "geom.hpp"
#include "tess.hpp"
#include "dict.hpp"
#include "priorityq.hpp"
#include "memalloc.hpp"
#include "sweep.hpp"

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#ifdef FOR_TRITE_TEST_PROGRAM
extern void DebugEvent( fastuidraw_GLUtesselator *tess );
#else
#define DebugEvent( tess )
#endif

/*
 * Invariants for the Edge Dictionary.
 * - each pair of adjacent edges e2=Succ(e1) satisfies EdgeLeq(e1,e2)
 *   at any valid location of the sweep event
 * - if EdgeLeq(e2,e1) as well (at any valid sweep event), then e1 and e2
 *   share a common endpoint
 * - for each e, e->Dst has been processed, but not e->Org
 * - each edge e satisfies VertLeq(e->Dst,event) && VertLeq(event,e->Org)
 *   where "event" is the current sweep line event.
 * - no edge e has zero length
 *
 * Invariants for the Mesh (the processed portion).
 * - the portion of the mesh left of the sweep line is a planar graph,
 *   ie. there is *some* way to embed it in the plane
 * - no processed edge has zero length
 * - no two processed vertices have identical coordinates
 * - each "inside" region is monotone, ie. can be broken into two chains
 *   of monotonically increasing vertices according to VertLeq(v1,v2)
 *   - a non-invariant: these chains may intersect (very slightly)
 *
 * Invariants for the Sweep.
 * - if none of the edges incident to the event vertex have an activeRegion
 *   (ie. none of these edges are in the edge dictionary), then the vertex
 *   has only right-going edges.
 * - if an edge is marked "fixUpperEdge" (it is a temporary edge introduced
 *   by ConnectRightVertex), then it is the only right-going edge from
 *   its associated vertex.  (This says that these edges exist only
 *   when it is necessary.)
 */

#undef  MAX
#undef  MIN
#define MAX(x,y)        ((x) >= (y) ? (x) : (y))
#define MIN(x,y)        ((x) <= (y) ? (x) : (y))

/* When we merge two edges into one, we need to compute the combined
 * winding of the new edge.
 */
#define AddWinding(eDst,eSrc)   (eDst->winding += eSrc->winding, \
                                 eDst->Sym->winding += eSrc->Sym->winding)

static void SweepEvent( fastuidraw_GLUtesselator *tess, GLUvertex *vEvent );
static void WalkDirtyRegions( fastuidraw_GLUtesselator *tess, ActiveRegion *regUp );
static int CheckForRightSplice( fastuidraw_GLUtesselator *tess, ActiveRegion *regUp );

static int EdgeLeq( fastuidraw_GLUtesselator *tess, ActiveRegion *reg1,
                    ActiveRegion *reg2 )
/*
 * Both edges must be directed from right to left (this is the canonical
 * direction for the upper edge of each region).
 *
 * The strategy is to evaluate a "t" value for each edge at the
 * current sweep line position, given by tess->event.  The calculations
 * are designed to be very stable, but of course they are not perfect.
 *
 * Special case: if both edge destinations are at the sweep event,
 * we sort the edges by slope (they would otherwise compare equally).
 */
{
  GLUvertex *event = tess->event;
  GLUhalfEdge *e1, *e2;
  double t1, t2;

  e1 = reg1->eUp;
  e2 = reg2->eUp;

  if( e1->Dst == event ) {
    if( e2->Dst == event ) {
      /* Two edges right of the sweep line which meet at the sweep event.
       * Sort them by slope.
       */
      if( VertLeq( e1->Org, e2->Org )) {
        return EdgeSign( e2->Dst, e1->Org, e2->Org ) <= 0;
      }
      return EdgeSign( e1->Dst, e2->Org, e1->Org ) >= 0;
    }
    return EdgeSign( e2->Dst, event, e2->Org ) <= 0;
  }
  if( e2->Dst == event ) {
    return EdgeSign( e1->Dst, event, e1->Org ) >= 0;
  }

  /* General case - compute signed distance *from* e1, e2 to event */
  t1 = EdgeEval( e1->Dst, event, e1->Org );
  t2 = EdgeEval( e2->Dst, event, e2->Org );
  return (t1 >= t2);
}


static void DeleteRegion( fastuidraw_GLUtesselator *tess, ActiveRegion *reg )
{
  if( reg->fixUpperEdge ) {
    /* It was created with zero winding number, so it better be
     * deleted with zero winding number (ie. it better not get merged
     * with a real edge).
     */
    FASTUIDRAWassert( reg->eUp->winding == 0 );
  }
  reg->eUp->activeRegion = nullptr;
  dictDelete( tess->dict, reg->nodeUp ); /* glu_fastuidraw_gl_dictListDelete */
  memFree( reg );
}


static int FixUpperEdge( ActiveRegion *reg, GLUhalfEdge *newEdge )
/*
 * Replace an upper edge which needs fixing (see ConnectRightVertex).
 */
{
  FASTUIDRAWassert( reg->fixUpperEdge );
  if ( !glu_fastuidraw_gl_meshDelete( reg->eUp ) ) return 0;
  reg->fixUpperEdge = FALSE;
  reg->eUp = newEdge;
  newEdge->activeRegion = reg;

  return 1;
}

static ActiveRegion *TopLeftRegion( ActiveRegion *reg )
{
  GLUvertex *org = reg->eUp->Org;
  GLUhalfEdge *e;

  /* Find the region above the uppermost edge with the same origin */
  do {
    reg = RegionAbove( reg );
  } while( reg->eUp->Org == org );

  /* If the edge above was a temporary edge introduced by ConnectRightVertex,
   * now is the time to fix it.
   */
  if( reg->fixUpperEdge ) {
    e = glu_fastuidraw_gl_meshConnect( RegionBelow(reg)->eUp->Sym, reg->eUp->Lnext );
    if (e == nullptr) return nullptr;
    if ( !FixUpperEdge( reg, e ) ) return nullptr;
    reg = RegionAbove( reg );
  }
  return reg;
}

static ActiveRegion *TopRightRegion( ActiveRegion *reg )
{
  GLUvertex *dst = reg->eUp->Dst;

  /* Find the region above the uppermost edge with the same destination */
  do {
    reg = RegionAbove( reg );
  } while( reg->eUp->Dst == dst );
  return reg;
}

static ActiveRegion *AddRegionBelow( fastuidraw_GLUtesselator *tess,
                                     ActiveRegion *regAbove,
                                     GLUhalfEdge *eNewUp )
/*
 * Add a new active region to the sweep line, *somewhere* below "regAbove"
 * (according to where the new edge belongs in the sweep-line dictionary).
 * The upper edge of the new region will be "eNewUp".
 * Winding number and "inside" flag are not updated.
 */
{
  ActiveRegion *regNew = (ActiveRegion *)memAlloc( sizeof( ActiveRegion ));
  if (regNew == nullptr) longjmp(tess->env,1);

  regNew->eUp = eNewUp;
  /* glu_fastuidraw_gl_dictListInsertBefore */
  regNew->nodeUp = dictInsertBefore( tess->dict, regAbove->nodeUp, regNew );
  if (regNew->nodeUp == nullptr) longjmp(tess->env,1);
  regNew->fixUpperEdge = FALSE;
  regNew->sentinel = FALSE;
  regNew->dirty = FALSE;

  eNewUp->activeRegion = regNew;
  return regNew;
}

static FASTUIDRAW_GLUboolean IsWindingInside( fastuidraw_GLUtesselator *tess, int n )
{
  return CALL_TESS_WINDING_OR_WINDING_DATA(n);
}


static void ComputeWinding( fastuidraw_GLUtesselator *tess, ActiveRegion *reg )
{
  reg->windingNumber = RegionAbove(reg)->windingNumber + reg->eUp->winding;
  reg->inside = IsWindingInside( tess, reg->windingNumber );
}


static void FinishRegion( fastuidraw_GLUtesselator *tess, ActiveRegion *reg )
/*
 * Delete a region from the sweep line.  This happens when the upper
 * and lower chains of a region meet (at a vertex on the sweep line).
 * The "inside" flag is copied to the appropriate mesh face (we could
 * not do this before -- since the structure of the mesh is always
 * changing, this face may not have even existed until now).
 */
{
  GLUhalfEdge *e = reg->eUp;
  GLUface *f = e->Lface;



  f->inside = reg->inside;
  f->winding_number = reg->windingNumber;

  if(CALL_TESS_WINDING_OR_WINDING_DATA(f->winding_number) && !f->inside)
    {
      /*
        [FASTUIDRAW-TODO]:
        Look at each of the vertices of the Face,
        if any of the vertices are marked as "DO NOT TRIANGULATE".
        This essentially means iterating over the vertex linked
        list f->next checking each of their Org to see if that
        vertex is a DO NOT TRIANGULATE vertex.
      */
    }


  f->anEdge = e;   /* optimization for glu_fastuidraw_gl_meshTessellateMonoRegion() */
  DeleteRegion( tess, reg );
}


static GLUhalfEdge *FinishLeftRegions( fastuidraw_GLUtesselator *tess,
               ActiveRegion *regFirst, ActiveRegion *regLast )
/*
 * We are given a vertex with one or more left-going edges.  All affected
 * edges should be in the edge dictionary.  Starting at regFirst->eUp,
 * we walk down deleting all regions where both edges have the same
 * origin vOrg.  At the same time we copy the "inside" flag from the
 * active region to the face, since at this point each face will belong
 * to at most one region (this was not necessarily true until this point
 * in the sweep).  The walk stops at the region above regLast; if regLast
 * is nullptr we walk as far as possible.  At the same time we relink the
 * mesh if necessary, so that the ordering of edges around vOrg is the
 * same as in the dictionary.
 */
{
  ActiveRegion *reg, *regPrev;
  GLUhalfEdge *e, *ePrev;

  regPrev = regFirst;
  ePrev = regFirst->eUp;
  while( regPrev != regLast ) {
    regPrev->fixUpperEdge = FALSE;      /* placement was OK */
    reg = RegionBelow( regPrev );
    e = reg->eUp;
    if( e->Org != ePrev->Org ) {
      if( ! reg->fixUpperEdge ) {
        /* Remove the last left-going edge.  Even though there are no further
         * edges in the dictionary with this origin, there may be further
         * such edges in the mesh (if we are adding left edges to a vertex
         * that has already been processed).  Thus it is important to call
         * FinishRegion rather than just DeleteRegion.
         */
        FinishRegion( tess, regPrev );
        break;
      }
      /* If the edge below was a temporary edge introduced by
       * ConnectRightVertex, now is the time to fix it.
       */
      e = glu_fastuidraw_gl_meshConnect( ePrev->Lprev, e->Sym );
      if (e == nullptr) longjmp(tess->env,1);
      if ( !FixUpperEdge( reg, e ) ) longjmp(tess->env,1);
    }

    /* Relink edges so that ePrev->Onext == e */
    if( ePrev->Onext != e ) {
      if ( !glu_fastuidraw_gl_meshSplice( e->Oprev, e ) ) longjmp(tess->env,1);
      if ( !glu_fastuidraw_gl_meshSplice( ePrev, e ) ) longjmp(tess->env,1);
    }
    FinishRegion( tess, regPrev );      /* may change reg->eUp */
    ePrev = reg->eUp;
    regPrev = reg;
  }
  return ePrev;
}


static void AddRightEdges( fastuidraw_GLUtesselator *tess, ActiveRegion *regUp,
       GLUhalfEdge *eFirst, GLUhalfEdge *eLast, GLUhalfEdge *eTopLeft,
       FASTUIDRAW_GLUboolean cleanUp )
/*
 * Purpose: insert right-going edges into the edge dictionary, and update
 * winding numbers and mesh connectivity appropriately.  All right-going
 * edges share a common origin vOrg.  Edges are inserted CCW starting at
 * eFirst; the last edge inserted is eLast->Oprev.  If vOrg has any
 * left-going edges already processed, then eTopLeft must be the edge
 * such that an imaginary upward vertical segment from vOrg would be
 * contained between eTopLeft->Oprev and eTopLeft; otherwise eTopLeft
 * should be nullptr.
 */
{
  ActiveRegion *reg, *regPrev;
  GLUhalfEdge *e, *ePrev;
  int firstTime = TRUE;

  /* Insert the new right-going edges in the dictionary */
  e = eFirst;
  do {
    FASTUIDRAWassert( VertLeq( e->Org, e->Dst ));
    AddRegionBelow( tess, regUp, e->Sym );
    e = e->Onext;
  } while ( e != eLast );

  /* Walk *all* right-going edges from e->Org, in the dictionary order,
   * updating the winding numbers of each region, and re-linking the mesh
   * edges to match the dictionary ordering (if necessary).
   */
  if( eTopLeft == nullptr ) {
    eTopLeft = RegionBelow( regUp )->eUp->Rprev;
  }
  regPrev = regUp;
  ePrev = eTopLeft;
  for( ;; ) {
    reg = RegionBelow( regPrev );
    e = reg->eUp->Sym;
    if( e->Org != ePrev->Org ) break;

    if( e->Onext != ePrev ) {
      /* Unlink e from its current position, and relink below ePrev */
      if ( !glu_fastuidraw_gl_meshSplice( e->Oprev, e ) ) longjmp(tess->env,1);
      if ( !glu_fastuidraw_gl_meshSplice( ePrev->Oprev, e ) ) longjmp(tess->env,1);
    }
    /* Compute the winding number and "inside" flag for the new regions */
    reg->windingNumber = regPrev->windingNumber - e->winding;
    reg->inside = IsWindingInside( tess, reg->windingNumber );

    /* Check for two outgoing edges with same slope -- process these
     * before any intersection tests (see example in glu_fastuidraw_gl_computeInterior).
     */
    regPrev->dirty = TRUE;
    if( ! firstTime && CheckForRightSplice( tess, regPrev )) {
      AddWinding( e, ePrev );
      DeleteRegion( tess, regPrev );
      if ( !glu_fastuidraw_gl_meshDelete( ePrev ) ) longjmp(tess->env,1);
    }
    firstTime = FALSE;
    regPrev = reg;
    ePrev = e;
  }
  regPrev->dirty = TRUE;
  FASTUIDRAWassert( regPrev->windingNumber - e->winding == reg->windingNumber );

  if( cleanUp ) {
    /* Check for intersections between newly adjacent edges. */
    WalkDirtyRegions( tess, regPrev );
  }
}


static void CallCombine( fastuidraw_GLUtesselator *tess, GLUvertex *isect,
                         unsigned int data[4], double weights[4], int needed )
{
  double x, y;

  /* Copy coord data in case the callback changes it. */
  x = isect->s;
  y = isect->t;

  if( needed
      && data[0] != FASTUIDRAW_GLU_nullptr_CLIENT_ID
      && data[1] != FASTUIDRAW_GLU_nullptr_CLIENT_ID
      && data[2] != FASTUIDRAW_GLU_nullptr_CLIENT_ID
      && data[3] != FASTUIDRAW_GLU_nullptr_CLIENT_ID) {
    isect->client_id = FASTUIDRAW_GLU_nullptr_CLIENT_ID;
    CALL_COMBINE_OR_COMBINE_DATA( x, y, data, weights, &isect->client_id );
  }
}

static void SpliceMergeVertices( fastuidraw_GLUtesselator *tess, GLUhalfEdge *e1,
                                 GLUhalfEdge *e2 )
/*
 * Two vertices with idential coordinates are combined into one.
 * e1->Org is kept, while e2->Org is discarded.
 */
{
  unsigned int data[4] =
    {
      FASTUIDRAW_GLU_nullptr_CLIENT_ID,
      FASTUIDRAW_GLU_nullptr_CLIENT_ID,
      FASTUIDRAW_GLU_nullptr_CLIENT_ID,
      FASTUIDRAW_GLU_nullptr_CLIENT_ID
    };

  double weights[4] = { 0.5, 0.5, 0.0, 0.0 };

  data[0] = e1->Org->client_id;
  data[1] = e2->Org->client_id;
  CallCombine( tess, e1->Org, data, weights, FALSE );
  if ( !glu_fastuidraw_gl_meshSplice( e1, e2 ) ) longjmp(tess->env,1);
}

static void VertexWeights( GLUvertex *isect, GLUvertex *org, GLUvertex *dst,
                           double *weights )
/*
 * Find some weights which describe how the intersection vertex is
 * a linear combination of "org" and "dest".  Each of the two edges
 * which generated "isect" is allocated 50% of the weight; each edge
 * splits the weight between its org and dst according to the
 * relative distance to "isect".
 */
{
  double t1 = VertL1dist( org, isect );
  double t2 = VertL1dist( dst, isect );

  weights[0] = 0.5 * t2 / (t1 + t2);
  weights[1] = 0.5 * t1 / (t1 + t2);
}


static void GetIntersectData( fastuidraw_GLUtesselator *tess, GLUvertex *isect,
                              GLUvertex *orgUp, GLUvertex *dstUp,
                              GLUvertex *orgLo, GLUvertex *dstLo )
/*
 * We've computed a new intersection point, now we need a "data" pointer
 * from the user so that we can refer to this new vertex in the
 * rendering callbacks.
 */
{
  unsigned int data[4];
  double weights[4];

  data[0] = orgUp->client_id;
  data[1] = dstUp->client_id;
  data[2] = orgLo->client_id;
  data[3] = dstLo->client_id;

  VertexWeights( isect, orgUp, dstUp, &weights[0] );
  VertexWeights( isect, orgLo, dstLo, &weights[2] );
  CallCombine( tess, isect, data, weights, TRUE );
}

static int CheckForRightSplice( fastuidraw_GLUtesselator *tess, ActiveRegion *regUp )
/*
 * Check the upper and lower edge of "regUp", to make sure that the
 * eUp->Org is above eLo, or eLo->Org is below eUp (depending on which
 * origin is leftmost).
 *
 * The main purpose is to splice right-going edges with the same
 * dest vertex and nearly identical slopes (ie. we can't distinguish
 * the slopes numerically).  However the splicing can also help us
 * to recover from numerical errors.  For example, suppose at one
 * point we checked eUp and eLo, and decided that eUp->Org is barely
 * above eLo.  Then later, we split eLo into two edges (eg. from
 * a splice operation like this one).  This can change the result of
 * our test so that now eUp->Org is incident to eLo, or barely below it.
 * We must correct this condition to maintain the dictionary invariants.
 *
 * One possibility is to check these edges for intersection again
 * (ie. CheckForIntersect).  This is what we do if possible.  However
 * CheckForIntersect requires that tess->event lies between eUp and eLo,
 * so that it has something to fall back on when the intersection
 * calculation gives us an unusable answer.  So, for those cases where
 * we can't check for intersection, this routine fixes the problem
 * by just splicing the offending vertex into the other edge.
 * This is a guaranteed solution, no matter how degenerate things get.
 * Basically this is a combinatorial solution to a numerical problem.
 */
{
  ActiveRegion *regLo = RegionBelow(regUp);
  GLUhalfEdge *eUp = regUp->eUp;
  GLUhalfEdge *eLo = regLo->eUp;

  if( VertLeq( eUp->Org, eLo->Org )) {
    if( EdgeSign( eLo->Dst, eUp->Org, eLo->Org ) > 0 ) return FALSE;

    /* eUp->Org appears to be below eLo */
    if( ! VertEq( eUp->Org, eLo->Org )) {
      /* Splice eUp->Org into eLo */
      if ( glu_fastuidraw_gl_meshSplitEdge( eLo->Sym ) == nullptr) longjmp(tess->env,1);
      if ( !glu_fastuidraw_gl_meshSplice( eUp, eLo->Oprev ) ) longjmp(tess->env,1);
      regUp->dirty = regLo->dirty = TRUE;

    } else if( eUp->Org != eLo->Org ) {
      /* merge the two vertices, discarding eUp->Org */
      pqDelete( tess->pq, eUp->Org->pqHandle ); /* glu_fastuidraw_gl_pqSortDelete */
      SpliceMergeVertices( tess, eLo->Oprev, eUp );
    }
  } else {
    if( EdgeSign( eUp->Dst, eLo->Org, eUp->Org ) < 0 ) return FALSE;

    /* eLo->Org appears to be above eUp, so splice eLo->Org into eUp */
    RegionAbove(regUp)->dirty = regUp->dirty = TRUE;
    if (glu_fastuidraw_gl_meshSplitEdge( eUp->Sym ) == nullptr) longjmp(tess->env,1);
    if ( !glu_fastuidraw_gl_meshSplice( eLo->Oprev, eUp ) ) longjmp(tess->env,1);
  }
  return TRUE;
}

static int CheckForLeftSplice( fastuidraw_GLUtesselator *tess, ActiveRegion *regUp )
/*
 * Check the upper and lower edge of "regUp", to make sure that the
 * eUp->Dst is above eLo, or eLo->Dst is below eUp (depending on which
 * destination is rightmost).
 *
 * Theoretically, this should always be true.  However, splitting an edge
 * into two pieces can change the results of previous tests.  For example,
 * suppose at one point we checked eUp and eLo, and decided that eUp->Dst
 * is barely above eLo.  Then later, we split eLo into two edges (eg. from
 * a splice operation like this one).  This can change the result of
 * the test so that now eUp->Dst is incident to eLo, or barely below it.
 * We must correct this condition to maintain the dictionary invariants
 * (otherwise new edges might get inserted in the wrong place in the
 * dictionary, and bad stuff will happen).
 *
 * We fix the problem by just splicing the offending vertex into the
 * other edge.
 */
{
  ActiveRegion *regLo = RegionBelow(regUp);
  GLUhalfEdge *eUp = regUp->eUp;
  GLUhalfEdge *eLo = regLo->eUp;
  GLUhalfEdge *e;

  FASTUIDRAWassert( ! VertEq( eUp->Dst, eLo->Dst ));

  if( VertLeq( eUp->Dst, eLo->Dst )) {
    if( EdgeSign( eUp->Dst, eLo->Dst, eUp->Org ) < 0 ) return FALSE;

    /* eLo->Dst is above eUp, so splice eLo->Dst into eUp */
    RegionAbove(regUp)->dirty = regUp->dirty = TRUE;
    e = glu_fastuidraw_gl_meshSplitEdge( eUp );
    if (e == nullptr) longjmp(tess->env,1);
    if ( !glu_fastuidraw_gl_meshSplice( eLo->Sym, e ) ) longjmp(tess->env,1);
    e->Lface->inside = regUp->inside;
  } else {
    if( EdgeSign( eLo->Dst, eUp->Dst, eLo->Org ) > 0 ) return FALSE;

    /* eUp->Dst is below eLo, so splice eUp->Dst into eLo */
    regUp->dirty = regLo->dirty = TRUE;
    e = glu_fastuidraw_gl_meshSplitEdge( eLo );
    if (e == nullptr) longjmp(tess->env,1);
    if ( !glu_fastuidraw_gl_meshSplice( eUp->Lnext, eLo->Sym ) ) longjmp(tess->env,1);
    e->Rface->inside = regUp->inside;
  }
  return TRUE;
}


static int CheckForIntersect( fastuidraw_GLUtesselator *tess, ActiveRegion *regUp )
/*
 * Check the upper and lower edges of the given region to see if
 * they intersect.  If so, create the intersection and add it
 * to the data structures.
 *
 * Returns TRUE if adding the new intersection resulted in a recursive
 * call to AddRightEdges(); in this case all "dirty" regions have been
 * checked for intersections, and possibly regUp has been deleted.
 */
{
  ActiveRegion *regLo = RegionBelow(regUp);
  GLUhalfEdge *eUp = regUp->eUp;
  GLUhalfEdge *eLo = regLo->eUp;
  GLUvertex *orgUp = eUp->Org;
  GLUvertex *orgLo = eLo->Org;
  GLUvertex *dstUp = eUp->Dst;
  GLUvertex *dstLo = eLo->Dst;
  double tMinUp, tMaxLo;
  GLUvertex isect, *orgMin;
  GLUhalfEdge *e;

  FASTUIDRAWassert( ! VertEq( dstLo, dstUp ));
  FASTUIDRAWassert( EdgeSign( dstUp, tess->event, orgUp ) <= 0 );
  FASTUIDRAWassert( EdgeSign( dstLo, tess->event, orgLo ) >= 0 );
  FASTUIDRAWassert( orgUp != tess->event && orgLo != tess->event );
  FASTUIDRAWassert( ! regUp->fixUpperEdge && ! regLo->fixUpperEdge );

  if( orgUp == orgLo ) return FALSE;    /* right endpoints are the same */

  tMinUp = MIN( orgUp->t, dstUp->t );
  tMaxLo = MAX( orgLo->t, dstLo->t );
  if( tMinUp > tMaxLo ) return FALSE;   /* t ranges do not overlap */

  if( VertLeq( orgUp, orgLo )) {
    if( EdgeSign( dstLo, orgUp, orgLo ) > 0 ) return FALSE;
  } else {
    if( EdgeSign( dstUp, orgLo, orgUp ) < 0 ) return FALSE;
  }

  /* At this point the edges intersect, at least marginally */
  DebugEvent( tess );

  glu_fastuidraw_gl_edgeIntersect( dstUp, orgUp, dstLo, orgLo, &isect );
  /* The following properties are guaranteed: */
  FASTUIDRAWassert( MIN( orgUp->t, dstUp->t ) <= isect.t );
  FASTUIDRAWassert( isect.t <= MAX( orgLo->t, dstLo->t ));
  FASTUIDRAWassert( MIN( dstLo->s, dstUp->s ) <= isect.s );
  FASTUIDRAWassert( isect.s <= MAX( orgLo->s, orgUp->s ));

  if( VertLeq( &isect, tess->event )) {
    /* The intersection point lies slightly to the left of the sweep line,
     * so move it until it''s slightly to the right of the sweep line.
     * (If we had perfect numerical precision, this would never happen
     * in the first place).  The easiest and safest thing to do is
     * replace the intersection by tess->event.
     */
    isect.s = tess->event->s;
    isect.t = tess->event->t;
  }
  /* Similarly, if the computed intersection lies to the right of the
   * rightmost origin (which should rarely happen), it can cause
   * unbelievable inefficiency on sufficiently degenerate inputs.
   * (If you have the test program, try running test54.d with the
   * "X zoom" option turned on).
   */
  orgMin = VertLeq( orgUp, orgLo ) ? orgUp : orgLo;
  if( VertLeq( orgMin, &isect )) {
    isect.s = orgMin->s;
    isect.t = orgMin->t;
  }

  if( VertEq( &isect, orgUp ) || VertEq( &isect, orgLo )) {
    /* Easy case -- intersection at one of the right endpoints */
    (void) CheckForRightSplice( tess, regUp );
    return FALSE;
  }

  if(    (! VertEq( dstUp, tess->event )
          && EdgeSign( dstUp, tess->event, &isect ) >= 0)
      || (! VertEq( dstLo, tess->event )
          && EdgeSign( dstLo, tess->event, &isect ) <= 0 ))
  {
    /* Very unusual -- the new upper or lower edge would pass on the
     * wrong side of the sweep event, or through it.  This can happen
     * due to very small numerical errors in the intersection calculation.
     */
    if( dstLo == tess->event ) {
      /* Splice dstLo into eUp, and process the new region(s) */
      if (glu_fastuidraw_gl_meshSplitEdge( eUp->Sym ) == nullptr) longjmp(tess->env,1);
      if ( !glu_fastuidraw_gl_meshSplice( eLo->Sym, eUp ) ) longjmp(tess->env,1);
      regUp = TopLeftRegion( regUp );
      if (regUp == nullptr) longjmp(tess->env,1);
      eUp = RegionBelow(regUp)->eUp;
      FinishLeftRegions( tess, RegionBelow(regUp), regLo );
      AddRightEdges( tess, regUp, eUp->Oprev, eUp, eUp, TRUE );
      return TRUE;
    }
    if( dstUp == tess->event ) {
      /* Splice dstUp into eLo, and process the new region(s) */
      if (glu_fastuidraw_gl_meshSplitEdge( eLo->Sym ) == nullptr) longjmp(tess->env,1);
      if ( !glu_fastuidraw_gl_meshSplice( eUp->Lnext, eLo->Oprev ) ) longjmp(tess->env,1);
      regLo = regUp;
      regUp = TopRightRegion( regUp );
      e = RegionBelow(regUp)->eUp->Rprev;
      regLo->eUp = eLo->Oprev;
      eLo = FinishLeftRegions( tess, regLo, nullptr );
      AddRightEdges( tess, regUp, eLo->Onext, eUp->Rprev, e, TRUE );
      return TRUE;
    }
    /* Special case: called from ConnectRightVertex.  If either
     * edge passes on the wrong side of tess->event, split it
     * (and wait for ConnectRightVertex to splice it appropriately).
     */
    if( EdgeSign( dstUp, tess->event, &isect ) >= 0 ) {
      RegionAbove(regUp)->dirty = regUp->dirty = TRUE;
      if (glu_fastuidraw_gl_meshSplitEdge( eUp->Sym ) == nullptr) longjmp(tess->env,1);
      eUp->Org->s = tess->event->s;
      eUp->Org->t = tess->event->t;
    }
    if( EdgeSign( dstLo, tess->event, &isect ) <= 0 ) {
      regUp->dirty = regLo->dirty = TRUE;
      if (glu_fastuidraw_gl_meshSplitEdge( eLo->Sym ) == nullptr) longjmp(tess->env,1);
      eLo->Org->s = tess->event->s;
      eLo->Org->t = tess->event->t;
    }
    /* leave the rest for ConnectRightVertex */
    return FALSE;
  }

  /* General case -- split both edges, splice into new vertex.
   * When we do the splice operation, the order of the arguments is
   * arbitrary as far as correctness goes.  However, when the operation
   * creates a new face, the work done is proportional to the size of
   * the new face.  We expect the faces in the processed part of
   * the mesh (ie. eUp->Lface) to be smaller than the faces in the
   * unprocessed original contours (which will be eLo->Oprev->Lface).
   */
  if (glu_fastuidraw_gl_meshSplitEdge( eUp->Sym ) == nullptr) longjmp(tess->env,1);
  if (glu_fastuidraw_gl_meshSplitEdge( eLo->Sym ) == nullptr) longjmp(tess->env,1);
  if ( !glu_fastuidraw_gl_meshSplice( eLo->Oprev, eUp ) ) longjmp(tess->env,1);
  eUp->Org->s = isect.s;
  eUp->Org->t = isect.t;
  eUp->Org->pqHandle = pqInsert( tess->pq, eUp->Org ); /* glu_fastuidraw_gl_pqSortInsert */
  if (eUp->Org->pqHandle == LONG_MAX) {
     pqDeletePriorityQ(tess->pq);       /* glu_fastuidraw_gl_pqSortDeletePriorityQ */
     tess->pq = nullptr;
     longjmp(tess->env,1);
  }
  GetIntersectData( tess, eUp->Org, orgUp, dstUp, orgLo, dstLo );
  RegionAbove(regUp)->dirty = regUp->dirty = regLo->dirty = TRUE;
  return FALSE;
}

static void WalkDirtyRegions( fastuidraw_GLUtesselator *tess, ActiveRegion *regUp )
/*
 * When the upper or lower edge of any region changes, the region is
 * marked "dirty".  This routine walks through all the dirty regions
 * and makes sure that the dictionary invariants are satisfied
 * (see the comments at the beginning of this file).  Of course
 * new dirty regions can be created as we make changes to restore
 * the invariants.
 */
{
  ActiveRegion *regLo = RegionBelow(regUp);
  GLUhalfEdge *eUp, *eLo;

  for( ;; ) {
    /* Find the lowest dirty region (we walk from the bottom up). */
    while( regLo->dirty ) {
      regUp = regLo;
      regLo = RegionBelow(regLo);
    }
    if( ! regUp->dirty ) {
      regLo = regUp;
      regUp = RegionAbove( regUp );
      if( regUp == nullptr || ! regUp->dirty ) {
        /* We've walked all the dirty regions */
        return;
      }
    }
    regUp->dirty = FALSE;
    eUp = regUp->eUp;
    eLo = regLo->eUp;

    if( eUp->Dst != eLo->Dst ) {
      /* Check that the edge ordering is obeyed at the Dst vertices. */
      if( CheckForLeftSplice( tess, regUp )) {

        /* If the upper or lower edge was marked fixUpperEdge, then
         * we no longer need it (since these edges are needed only for
         * vertices which otherwise have no right-going edges).
         */
        if( regLo->fixUpperEdge ) {
          DeleteRegion( tess, regLo );
          if ( !glu_fastuidraw_gl_meshDelete( eLo ) ) longjmp(tess->env,1);
          regLo = RegionBelow( regUp );
          eLo = regLo->eUp;
        } else if( regUp->fixUpperEdge ) {
          DeleteRegion( tess, regUp );
          if ( !glu_fastuidraw_gl_meshDelete( eUp ) ) longjmp(tess->env,1);
          regUp = RegionAbove( regLo );
          eUp = regUp->eUp;
        }
      }
    }
    if( eUp->Org != eLo->Org ) {
      if(    eUp->Dst != eLo->Dst
          && ! regUp->fixUpperEdge && ! regLo->fixUpperEdge
          && (eUp->Dst == tess->event || eLo->Dst == tess->event) )
      {
        /* When all else fails in CheckForIntersect(), it uses tess->event
         * as the intersection location.  To make this possible, it requires
         * that tess->event lie between the upper and lower edges, and also
         * that neither of these is marked fixUpperEdge (since in the worst
         * case it might splice one of these edges into tess->event, and
         * violate the invariant that fixable edges are the only right-going
         * edge from their associated vertex).
         */
        if( CheckForIntersect( tess, regUp )) {
          /* WalkDirtyRegions() was called recursively; we're done */
          return;
        }
      } else {
        /* Even though we can't use CheckForIntersect(), the Org vertices
         * may violate the dictionary edge ordering.  Check and correct this.
         */
        (void) CheckForRightSplice( tess, regUp );
      }
    }
    if( eUp->Org == eLo->Org && eUp->Dst == eLo->Dst ) {
      /* A degenerate loop consisting of only two edges -- delete it. */
      AddWinding( eLo, eUp );
      DeleteRegion( tess, regUp );
      if ( !glu_fastuidraw_gl_meshDelete( eUp ) ) longjmp(tess->env,1);
      regUp = RegionAbove( regLo );
    }
  }
}


static void ConnectRightVertex( fastuidraw_GLUtesselator *tess, ActiveRegion *regUp,
                                GLUhalfEdge *eBottomLeft )
/*
 * Purpose: connect a "right" vertex vEvent (one where all edges go left)
 * to the unprocessed portion of the mesh.  Since there are no right-going
 * edges, two regions (one above vEvent and one below) are being merged
 * into one.  "regUp" is the upper of these two regions.
 *
 * There are two reasons for doing this (adding a right-going edge):
 *  - if the two regions being merged are "inside", we must add an edge
 *    to keep them separated (the combined region would not be monotone).
 *  - in any case, we must leave some record of vEvent in the dictionary,
 *    so that we can merge vEvent with features that we have not seen yet.
 *    For example, maybe there is a vertical edge which passes just to
 *    the right of vEvent; we would like to splice vEvent into this edge.
 *
 * However, we don't want to connect vEvent to just any vertex.  We don''t
 * want the new edge to cross any other edges; otherwise we will create
 * intersection vertices even when the input data had no self-intersections.
 * (This is a bad thing; if the user's input data has no intersections,
 * we don't want to generate any false intersections ourselves.)
 *
 * Our eventual goal is to connect vEvent to the leftmost unprocessed
 * vertex of the combined region (the union of regUp and regLo).
 * But because of unseen vertices with all right-going edges, and also
 * new vertices which may be created by edge intersections, we don''t
 * know where that leftmost unprocessed vertex is.  In the meantime, we
 * connect vEvent to the closest vertex of either chain, and mark the region
 * as "fixUpperEdge".  This flag says to delete and reconnect this edge
 * to the next processed vertex on the boundary of the combined region.
 * Quite possibly the vertex we connected to will turn out to be the
 * closest one, in which case we won''t need to make any changes.
 */
{
  GLUhalfEdge *eNew;
  GLUhalfEdge *eTopLeft = eBottomLeft->Onext;
  ActiveRegion *regLo = RegionBelow(regUp);
  GLUhalfEdge *eUp = regUp->eUp;
  GLUhalfEdge *eLo = regLo->eUp;
  int degenerate = FALSE;

  if( eUp->Dst != eLo->Dst ) {
    (void) CheckForIntersect( tess, regUp );
  }

  /* Possible new degeneracies: upper or lower edge of regUp may pass
   * through vEvent, or may coincide with new intersection vertex
   */
  if( VertEq( eUp->Org, tess->event )) {
    if ( !glu_fastuidraw_gl_meshSplice( eTopLeft->Oprev, eUp ) ) longjmp(tess->env,1);
    regUp = TopLeftRegion( regUp );
    if (regUp == nullptr) longjmp(tess->env,1);
    eTopLeft = RegionBelow( regUp )->eUp;
    FinishLeftRegions( tess, RegionBelow(regUp), regLo );
    degenerate = TRUE;
  }
  if( VertEq( eLo->Org, tess->event )) {
    if ( !glu_fastuidraw_gl_meshSplice( eBottomLeft, eLo->Oprev ) ) longjmp(tess->env,1);
    eBottomLeft = FinishLeftRegions( tess, regLo, nullptr );
    degenerate = TRUE;
  }
  if( degenerate ) {
    AddRightEdges( tess, regUp, eBottomLeft->Onext, eTopLeft, eTopLeft, TRUE );
    return;
  }

  /* Non-degenerate situation -- need to add a temporary, fixable edge.
   * Connect to the closer of eLo->Org, eUp->Org.
   */
  if( VertLeq( eLo->Org, eUp->Org )) {
    eNew = eLo->Oprev;
  } else {
    eNew = eUp;
  }
  eNew = glu_fastuidraw_gl_meshConnect( eBottomLeft->Lprev, eNew );
  if (eNew == nullptr) longjmp(tess->env,1);

  /* Prevent cleanup, otherwise eNew might disappear before we've even
   * had a chance to mark it as a temporary edge.
   */
  AddRightEdges( tess, regUp, eNew, eNew->Onext, eNew->Onext, FALSE );
  eNew->Sym->activeRegion->fixUpperEdge = TRUE;
  WalkDirtyRegions( tess, regUp );
}

/* Because vertices at exactly the same location are merged together
 * before we process the sweep event, some degenerate cases can't occur.
 * However if someone eventually makes the modifications required to
 * merge features which are close together, the cases below marked
 * TOLERANCE_NONZERO will be useful.  They were debugged before the
 * code to merge identical vertices in the main loop was added.
 */
#define TOLERANCE_NONZERO       FALSE

static void ConnectLeftDegenerate( fastuidraw_GLUtesselator *tess,
                                   ActiveRegion *regUp, GLUvertex *vEvent )
/*
 * The event vertex lies exacty on an already-processed edge or vertex.
 * Adding the new vertex involves splicing it into the already-processed
 * part of the mesh.
 */
{
  GLUhalfEdge *e, *eTopLeft, *eTopRight, *eLast;
  ActiveRegion *reg;

  e = regUp->eUp;
  if( VertEq( e->Org, vEvent )) {
    /* e->Org is an unprocessed vertex - just combine them, and wait
     * for e->Org to be pulled from the queue
     */
    FASTUIDRAWassert( TOLERANCE_NONZERO );
    SpliceMergeVertices( tess, e, vEvent->anEdge );
    return;
  }

  if( ! VertEq( e->Dst, vEvent )) {
    /* General case -- splice vEvent into edge e which passes through it */
    if (glu_fastuidraw_gl_meshSplitEdge( e->Sym ) == nullptr) longjmp(tess->env,1);
    if( regUp->fixUpperEdge ) {
      /* This edge was fixable -- delete unused portion of original edge */
      if ( !glu_fastuidraw_gl_meshDelete( e->Onext ) ) longjmp(tess->env,1);
      regUp->fixUpperEdge = FALSE;
    }
    if ( !glu_fastuidraw_gl_meshSplice( vEvent->anEdge, e ) ) longjmp(tess->env,1);
    SweepEvent( tess, vEvent ); /* recurse */
    return;
  }

  /* vEvent coincides with e->Dst, which has already been processed.
   * Splice in the additional right-going edges.
   */
  FASTUIDRAWassert( TOLERANCE_NONZERO );
  regUp = TopRightRegion( regUp );
  reg = RegionBelow( regUp );
  eTopRight = reg->eUp->Sym;
  eTopLeft = eLast = eTopRight->Onext;
  if( reg->fixUpperEdge ) {
    /* Here e->Dst has only a single fixable edge going right.
     * We can delete it since now we have some real right-going edges.
     */
    FASTUIDRAWassert( eTopLeft != eTopRight );   /* there are some left edges too */
    DeleteRegion( tess, reg );
    if ( !glu_fastuidraw_gl_meshDelete( eTopRight ) ) longjmp(tess->env,1);
    eTopRight = eTopLeft->Oprev;
  }
  if ( !glu_fastuidraw_gl_meshSplice( vEvent->anEdge, eTopRight ) ) longjmp(tess->env,1);
  if( ! EdgeGoesLeft( eTopLeft )) {
    /* e->Dst had no left-going edges -- indicate this to AddRightEdges() */
    eTopLeft = nullptr;
  }
  AddRightEdges( tess, regUp, eTopRight->Onext, eLast, eTopLeft, TRUE );
}


static void ConnectLeftVertex( fastuidraw_GLUtesselator *tess, GLUvertex *vEvent )
/*
 * Purpose: connect a "left" vertex (one where both edges go right)
 * to the processed portion of the mesh.  Let R be the active region
 * containing vEvent, and let U and L be the upper and lower edge
 * chains of R.  There are two possibilities:
 *
 * - the normal case: split R into two regions, by connecting vEvent to
 *   the rightmost vertex of U or L lying to the left of the sweep line
 *
 * - the degenerate case: if vEvent is close enough to U or L, we
 *   merge vEvent into that edge chain.  The subcases are:
 *      - merging with the rightmost vertex of U or L
 *      - merging with the active edge of U or L
 *      - merging with an already-processed portion of U or L
 */
{
  ActiveRegion *regUp, *regLo, *reg;
  GLUhalfEdge *eUp, *eLo, *eNew;
  ActiveRegion tmp;

  /* FASTUIDRAWassert( vEvent->anEdge->Onext->Onext == vEvent->anEdge ); */

  /* Get a pointer to the active region containing vEvent */
  tmp.eUp = vEvent->anEdge->Sym;
  /* __GL_DICTLISTKEY */ /* glu_fastuidraw_gl_dictListSearch */
  regUp = (ActiveRegion *)dictKey( dictSearch( tess->dict, &tmp ));
  regLo = RegionBelow( regUp );
  eUp = regUp->eUp;
  eLo = regLo->eUp;

  /* Try merging with U or L first */
  if( EdgeSign( eUp->Dst, vEvent, eUp->Org ) == 0 ) {
    ConnectLeftDegenerate( tess, regUp, vEvent );
    return;
  }

  /* Connect vEvent to rightmost processed vertex of either chain.
   * e->Dst is the vertex that we will connect to vEvent.
   */
  reg = VertLeq( eLo->Dst, eUp->Dst ) ? regUp : regLo;

  if( regUp->inside || reg->fixUpperEdge) {
    if( reg == regUp ) {
      eNew = glu_fastuidraw_gl_meshConnect( vEvent->anEdge->Sym, eUp->Lnext );
      if (eNew == nullptr) longjmp(tess->env,1);
    } else {
      GLUhalfEdge *tempHalfEdge= glu_fastuidraw_gl_meshConnect( eLo->Dnext, vEvent->anEdge);
      if (tempHalfEdge == nullptr) longjmp(tess->env,1);

      eNew = tempHalfEdge->Sym;
    }
    if( reg->fixUpperEdge ) {
      if ( !FixUpperEdge( reg, eNew ) ) longjmp(tess->env,1);
    } else {
      ComputeWinding( tess, AddRegionBelow( tess, regUp, eNew ));
    }
    SweepEvent( tess, vEvent );
  } else {
    /* The new vertex is in a region which does not belong to the polygon.
     * We don''t need to connect this vertex to the rest of the mesh.
     */
    AddRightEdges( tess, regUp, vEvent->anEdge, vEvent->anEdge, nullptr, TRUE );
  }
}


static void SweepEvent( fastuidraw_GLUtesselator *tess, GLUvertex *vEvent )
/*
 * Does everything necessary when the sweep line crosses a vertex.
 * Updates the mesh and the edge dictionary.
 */
{
  ActiveRegion *regUp, *reg;
  GLUhalfEdge *e, *eTopLeft, *eBottomLeft;

  tess->event = vEvent;         /* for access in EdgeLeq() */
  DebugEvent( tess );

  /* Check if this vertex is the right endpoint of an edge that is
   * already in the dictionary.  In this case we don't need to waste
   * time searching for the location to insert new edges.
   */
  e = vEvent->anEdge;
  while( e->activeRegion == nullptr ) {
    e = e->Onext;
    if( e == vEvent->anEdge ) {
      /* All edges go right -- not incident to any processed edges */
      ConnectLeftVertex( tess, vEvent );
      return;
    }
  }

  /* Processing consists of two phases: first we "finish" all the
   * active regions where both the upper and lower edges terminate
   * at vEvent (ie. vEvent is closing off these regions).
   * We mark these faces "inside" or "outside" the polygon according
   * to their winding number, and delete the edges from the dictionary.
   * This takes care of all the left-going edges from vEvent.
   */
  regUp = TopLeftRegion( e->activeRegion );
  if (regUp == nullptr) longjmp(tess->env,1);
  reg = RegionBelow( regUp );
  eTopLeft = reg->eUp;
  eBottomLeft = FinishLeftRegions( tess, reg, nullptr );

  /* Next we process all the right-going edges from vEvent.  This
   * involves adding the edges to the dictionary, and creating the
   * associated "active regions" which record information about the
   * regions between adjacent dictionary edges.
   */
  if( eBottomLeft->Onext == eTopLeft ) {
    /* No right-going edges -- add a temporary "fixable" edge */
    ConnectRightVertex( tess, regUp, eBottomLeft );
  } else {
    AddRightEdges( tess, regUp, eBottomLeft->Onext, eTopLeft, eTopLeft, TRUE );
  }
}


/* Make the sentinel coordinates big enough that they will never be
 * merged with real input features.  (Even with the largest possible
 * input contour and the maximum tolerance of 1.0, no merging will be
 * done with coordinates larger than 3 * FASTUIDRAW_GLU_TESS_MAX_COORD).
 */
#define SENTINEL_COORD  (4 * FASTUIDRAW_GLU_TESS_MAX_COORD)

static void AddSentinel( fastuidraw_GLUtesselator *tess, double t )
/*
 * We add two sentinel edges above and below all other edges,
 * to avoid special cases at the top and bottom.
 */
{
  GLUhalfEdge *e;
  ActiveRegion *reg = (ActiveRegion *)memAlloc( sizeof( ActiveRegion ));
  if (reg == nullptr) longjmp(tess->env,1);

  e = glu_fastuidraw_gl_meshMakeEdge( tess->mesh );
  if (e == nullptr) longjmp(tess->env,1);

  e->Org->s = SENTINEL_COORD;
  e->Org->t = t;
  e->Dst->s = -SENTINEL_COORD;
  e->Dst->t = t;
  tess->event = e->Dst;         /* initialize it */

  reg->eUp = e;
  reg->windingNumber = 0;
  reg->inside = FALSE;
  reg->fixUpperEdge = FALSE;
  reg->sentinel = TRUE;
  reg->dirty = FALSE;
  reg->nodeUp = dictInsert( tess->dict, reg ); /* glu_fastuidraw_gl_dictListInsertBefore */
  if (reg->nodeUp == nullptr) longjmp(tess->env,1);
}


static void InitEdgeDict( fastuidraw_GLUtesselator *tess )
/*
 * We maintain an ordering of edge intersections with the sweep line.
 * This order is maintained in a dynamic dictionary.
 */
{
  /* glu_fastuidraw_gl_dictListNewDict */
  tess->dict = dictNewDict( tess, (int (*)(void *, DictKey, DictKey)) EdgeLeq );
  if (tess->dict == nullptr) longjmp(tess->env,1);

  AddSentinel( tess, -SENTINEL_COORD );
  AddSentinel( tess, SENTINEL_COORD );
}


static void DoneEdgeDict( fastuidraw_GLUtesselator *tess )
{
  ActiveRegion *reg;
#ifndef NDEBUG
  int fixedEdges = 0;
#endif

  /* __GL_DICTLISTKEY */ /* __GL_DICTLISTMIN */
  while( (reg = (ActiveRegion *)dictKey( dictMin( tess->dict ))) != nullptr ) {
    /*
     * At the end of all processing, the dictionary should contain
     * only the two sentinel edges, plus at most one "fixable" edge
     * created by ConnectRightVertex().
     */
    if( ! reg->sentinel ) {
      FASTUIDRAWassert( reg->fixUpperEdge );
      FASTUIDRAWassert( ++fixedEdges == 1 );
    }
    FASTUIDRAWassert( reg->windingNumber == 0 );
    DeleteRegion( tess, reg );
/*    glu_fastuidraw_gl_meshDelete( reg->eUp );*/
  }
  dictDeleteDict( tess->dict ); /* glu_fastuidraw_gl_dictListDeleteDict */
}


static void RemoveDegenerateEdges( fastuidraw_GLUtesselator *tess )
/*
 * Remove zero-length edges, and contours with fewer than 3 vertices.
 */
{
  GLUhalfEdge *e, *eNext, *eLnext;
  GLUhalfEdge *eHead = &tess->mesh->eHead;

  /*LINTED*/
  for( e = eHead->next; e != eHead; e = eNext ) {
    eNext = e->next;
    eLnext = e->Lnext;

    if( VertEq( e->Org, e->Dst ) && e->Lnext->Lnext != e ) {
      /* Zero-length edge, contour has at least 3 edges */

      SpliceMergeVertices( tess, eLnext, e );   /* deletes e->Org */
      if ( !glu_fastuidraw_gl_meshDelete( e ) ) longjmp(tess->env,1); /* e is a self-loop */
      e = eLnext;
      eLnext = e->Lnext;
    }
    if( eLnext->Lnext == e ) {
      /* Degenerate contour (one or two edges) */

      if( eLnext != e ) {
        if( eLnext == eNext || eLnext == eNext->Sym ) { eNext = eNext->next; }
        if ( !glu_fastuidraw_gl_meshDelete( eLnext ) ) longjmp(tess->env,1);
      }
      if( e == eNext || e == eNext->Sym ) { eNext = eNext->next; }
      if ( !glu_fastuidraw_gl_meshDelete( e ) ) longjmp(tess->env,1);
    }
  }
}

static int InitPriorityQ( fastuidraw_GLUtesselator *tess )
/*
 * Insert all vertices into the priority queue which determines the
 * order in which vertices cross the sweep line.
 */
{
  PriorityQ *pq;
  GLUvertex *v, *vHead;

  /* glu_fastuidraw_gl_pqSortNewPriorityQ */
  pq = tess->pq = pqNewPriorityQ( (int (*)(PQkey, PQkey)) glu_fastuidraw_gl_vertLeq );
  if (pq == nullptr) return 0;

  vHead = &tess->mesh->vHead;
  for( v = vHead->next; v != vHead; v = v->next ) {
    v->pqHandle = pqInsert( pq, v ); /* glu_fastuidraw_gl_pqSortInsert */
    if (v->pqHandle == LONG_MAX) break;
  }
  if (v != vHead || !pqInit( pq ) ) { /* glu_fastuidraw_gl_pqSortInit */
    pqDeletePriorityQ(tess->pq);        /* glu_fastuidraw_gl_pqSortDeletePriorityQ */
    tess->pq = nullptr;
    return 0;
  }

  return 1;
}


static void DonePriorityQ( fastuidraw_GLUtesselator *tess )
{
  pqDeletePriorityQ( tess->pq ); /* glu_fastuidraw_gl_pqSortDeletePriorityQ */
}


static int RemoveDegenerateFaces( GLUmesh *mesh )
/*
 * Delete any degenerate faces with only two edges.  WalkDirtyRegions()
 * will catch almost all of these, but it won't catch degenerate faces
 * produced by splice operations on already-processed edges.
 * The two places this can happen are in FinishLeftRegions(), when
 * we splice in a "temporary" edge produced by ConnectRightVertex(),
 * and in CheckForLeftSplice(), where we splice already-processed
 * edges to ensure that our dictionary invariants are not violated
 * by numerical errors.
 *
 * In both these cases it is *very* dangerous to delete the offending
 * edge at the time, since one of the routines further up the stack
 * will sometimes be keeping a pointer to that edge.
 */
{
  GLUface *f, *fNext;
  GLUhalfEdge *e;

  /*LINTED*/
  for( f = mesh->fHead.next; f != &mesh->fHead; f = fNext ) {
    fNext = f->next;
    e = f->anEdge;
    FASTUIDRAWassert( e->Lnext != e );

    if( e->Lnext->Lnext == e ) {
      /* A face with only two edges */
      AddWinding( e->Onext, e );
      if ( !glu_fastuidraw_gl_meshDelete( e ) ) return 0;
    }
  }
  return 1;
}

int glu_fastuidraw_gl_computeInterior( fastuidraw_GLUtesselator *tess )
/*
 * glu_fastuidraw_gl_computeInterior( tess ) computes the planar arrangement specified
 * by the given contours, and further subdivides this arrangement
 * into regions.  Each region is marked "inside" if it belongs
 * to the polygon, according to the rule given by tess->windingRule.
 * Each interior region is guaranteed be monotone.
 */
{
  GLUvertex *v, *vNext;

  tess->fatalError = FALSE;

  /* Each vertex defines an event for our sweep line.  Start by inserting
   * all the vertices in a priority queue.  Events are processed in
   * lexicographic order, ie.
   *
   *    e1 < e2  iff  e1.x < e2.x || (e1.x == e2.x && e1.y < e2.y)
   */
  RemoveDegenerateEdges( tess );
  if ( !InitPriorityQ( tess ) ) return 0; /* if error */
  InitEdgeDict( tess );

  /* glu_fastuidraw_gl_pqSortExtractMin */
  while( (v = (GLUvertex *)pqExtractMin( tess->pq )) != nullptr ) {
    for( ;; ) {
      vNext = (GLUvertex *)pqMinimum( tess->pq ); /* glu_fastuidraw_gl_pqSortMinimum */
      if( vNext == nullptr || ! VertEq( vNext, v )) break;

      /* Merge together all vertices at exactly the same location.
       * This is more efficient than processing them one at a time,
       * simplifies the code (see ConnectLeftDegenerate), and is also
       * important for correct handling of certain degenerate cases.
       * For example, suppose there are two identical edges A and B
       * that belong to different contours (so without this code they would
       * be processed by separate sweep events).  Suppose another edge C
       * crosses A and B from above.  When A is processed, we split it
       * at its intersection point with C.  However this also splits C,
       * so when we insert B we may compute a slightly different
       * intersection point.  This might leave two edges with a small
       * gap between them.  This kind of error is especially obvious
       * when using boundary extraction (FASTUIDRAW_GLU_TESS_BOUNDARY_ONLY).
       */
      vNext = (GLUvertex *)pqExtractMin( tess->pq ); /* glu_fastuidraw_gl_pqSortExtractMin*/
      SpliceMergeVertices( tess, v->anEdge, vNext->anEdge );
    }
    SweepEvent( tess, v );
  }

  /* Set tess->event for debugging purposes */
  /* __GL_DICTLISTKEY */ /* __GL_DICTLISTMIN */
  tess->event = ((ActiveRegion *) dictKey( dictMin( tess->dict )))->eUp->Org;
  DebugEvent( tess );
  DoneEdgeDict( tess );
  DonePriorityQ( tess );

  if ( !RemoveDegenerateFaces( tess->mesh ) ) return 0;
  glu_fastuidraw_gl_meshCheckMesh( tess->mesh );

  return 1;
}
