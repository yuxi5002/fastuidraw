/*!
 * \file path.hpp
 * \brief file path.hpp
 *
 * Copyright 2016 by Intel.
 *
 * Contact: kevin.rogovin@intel.com
 *
 * This Source Code Form is subject to the
 * terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with
 * this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * \author Kevin Rogovin <kevin.rogovin@intel.com>
 *
 */



#pragma once

#include <fastuidraw/util/fastuidraw_memory.hpp>
#include <fastuidraw/util/vecN.hpp>
#include <fastuidraw/util/c_array.hpp>
#include <fastuidraw/util/reference_counted.hpp>
#include <fastuidraw/path_enums.hpp>
#include <fastuidraw/tessellated_path.hpp>

namespace fastuidraw  {

/*!\addtogroup Paths
 * @{
 */

/*!
 * \brief
 * An PathContour represents a single contour within
 * a Path.
 *
 * Ending a contour (see \ref end(), \ref
 * end_generic() and end_arc()) means to specify
 * the edge from the last point of the PathContour
 * to the first point.
 */
class PathContour:
    public reference_counted<PathContour>::non_concurrent
{
public:
  /*!
   * \brief
   * Provides an interface to resume from a previous tessellation
   * of a \ref interpolator_base derived object.
   */
  class tessellation_state:
    public reference_counted<tessellation_state>::non_concurrent
  {
  public:
    /*!
     * To be implemented by a derived class to return the depth
     * of recursion at this objects stage of tessellation.
     */
    virtual
    unsigned int
    recursion_depth(void) const = 0;

    /*!
     * To be implemented by a derived class to resume tessellation
     * and to (try to) achieve the required threshhold within the
     * recursion limits of a \ref TessellatedPath::TessellationParams
     * value.
     * \param tess_params tessellation parameters
     * \param out_data location to which to write the tessellations
     * \param out_max_distance location to which to write an upperbound for the
     *                         distance between the curve and the tesseallation
     *                         approximation.
     */
    virtual
    void
    resume_tessellation(const TessellatedPath::TessellationParams &tess_params,
                        TessellatedPath::SegmentStorage *out_data,
                        float *out_max_distance) = 0;
  };

  /*!
   * \brief
   * Base class to describe how to interpolate from one
   * point of a PathContour to the next, i.e. describes
   * the shape of an edge.
   */
  class interpolator_base:
    public reference_counted<interpolator_base>::non_concurrent
  {
  public:
    /*!
     * Ctor.
     * \param prev interpolator to edge that ends at start of edge
     *             of this interpolator
     * \param end end point of the edge of this interpolator
     * \param tp nature the edge represented by this interpolator_base
     */
    interpolator_base(const reference_counted_ptr<const interpolator_base> &prev,
                      const vec2 &end, enum PathEnums::edge_type_t tp);

    virtual
    ~interpolator_base();

    /*!
     * Returns the interpolator previous to this interpolator_base
     * within the PathContour that this object resides.
     */
    reference_counted_ptr<const interpolator_base>
    prev_interpolator(void) const;

    /*!
     * Returns the starting point of this interpolator.
     */
    const vec2&
    start_pt(void) const;

    /*!
     * Returns the ending point of this interpolator
     */
    const vec2&
    end_pt(void) const;

    /*!
     * Returns the edge type.
     */
    enum PathEnums::edge_type_t
    edge_type(void) const;

    /*!
     * To be implemented by a derived class to return true if
     * the interpolator is flat, i.e. is just a line segment
     * connecting start_pt() to end_pt().
     */
    virtual
    bool
    is_flat(void) const = 0;

    /*!
     * To be implemented by a derived class to produce the arc-tessellation
     * from start_pt() to end_pt(). In addition, for recursive tessellation,
     * returns the tessellation state to be queried for recursion depth and
     * reused to refine the tessellation. If the tessellation routine is not
     * recursive in nature, return nullptr.
     *
     * \param tess_params tessellation parameters
     * \param out_data location to which to write the tessellations
     * \param out_max_distance location to which to write an upperbound for the
     *                       distance between the curve and the tesseallation
     *                       approximation.
     */
    virtual
    reference_counted_ptr<tessellation_state>
    produce_tessellation(const TessellatedPath::TessellationParams &tess_params,
                         TessellatedPath::SegmentStorage *out_data,
                         float *out_max_distance) const = 0;

    /*!
     * To be implemented by a derived class to return a fast (and approximate)
     * bounding box for the interpolator.
     * \param out_min_bb (output) location to which to write the min-x and min-y
     *                            coordinates of the approximate bounding box.
     * \param out_max_bb (output) location to which to write the max-x and max-y
     *                            coordinates of the approximate bounding box.
     */
    virtual
    void
    approximate_bounding_box(vec2 *out_min_bb, vec2 *out_max_bb) const = 0;

    /*!
     * To be implemented by a derived class to create and
     * return a deep copy of the interpolator object.
     */
    virtual
    interpolator_base*
    deep_copy(const reference_counted_ptr<const interpolator_base> &prev) const = 0;

  private:
    friend class PathContour;
    void *m_d;
  };

  /*!
   * \brief
   * A flat interpolator represents a flat edge.
   */
  class flat:public interpolator_base
  {
  public:
    /*!
     * Ctor.
     * \param prev interpolator to edge that ends at start of edge
     *             of this interpolator
     * \param end end point of the edge of this interpolator
     * \param tp nature the edge represented by this interpolator_base
     */
    flat(const reference_counted_ptr<const interpolator_base> &prev,
         const vec2 &end, enum PathEnums::edge_type_t tp):
      interpolator_base(prev, end, tp)
    {}

    virtual
    bool
    is_flat(void) const;

    virtual
    reference_counted_ptr<tessellation_state>
    produce_tessellation(const TessellatedPath::TessellationParams &tess_params,
                         TessellatedPath::SegmentStorage *out_data,
                         float *out_max_distance) const;
    virtual
    void
    approximate_bounding_box(vec2 *out_min_bb, vec2 *out_max_bb) const;

    virtual
    interpolator_base*
    deep_copy(const reference_counted_ptr<const interpolator_base> &prev) const;
  };

  /*!
   * \brief
   * Interpolator generic implements tessellation by recursion
   * and relying on analytic derivative provided by derived
   * class.
   */
  class interpolator_generic:public interpolator_base
  {
  public:
    /*!
     * A tessellated_region is a base class for a cookie
     * used and generated by tessellate().
     */
    class tessellated_region:
      public reference_counted<tessellated_region>::non_concurrent
    {
    public:
      /*!
       * To be implemented by a derived class to compute an upper-bound
       * of the distance from the curve restricted to the region to the
       * line segment connecting the end points of the region.
       */
      virtual
      float
      distance_to_line_segment(void) const = 0;

      /*!
       * To be implemented by a derived class to compute an approximate
       * upper-bound for the distance from the curve restricted to the
       * region to a given arc.
       * \param arc_radius radius of the arc
       * \param center center of the circle of the arc
       * \param unit_vector_arc_middle unit vector from center to the midpoint of the arc
       * \param cos_half_arc_angle the cosine of half of the arc-angle
       */
      virtual
      float
      distance_to_arc(float arc_radius, vec2 center,
                      vec2 unit_vector_arc_middle,
                      float cos_half_arc_angle) const = 0;
    };

    /*!
     * Ctor.
     * \param prev interpolator to edge that ends at start of edge
     *             of this interpolator
     * \param end end point of the edge of this interpolator
     * \param tp nature the edge represented by this interpolator_base
     */
    interpolator_generic(const reference_counted_ptr<const interpolator_base> &prev,
                         const vec2 &end, enum PathEnums::edge_type_t tp):
      interpolator_base(prev, end, tp)
    {}

    virtual
    reference_counted_ptr<tessellation_state>
    produce_tessellation(const TessellatedPath::TessellationParams &tess_params,
                         TessellatedPath::SegmentStorage *out_data,
                         float *out_max_distance) const;

    /*!
     * To be implemented by a derived to assist in recursive tessellation.
     * \param in_region region to divide in half, a nullptr value indicates
     *                  that the region is the entire interpolator.
     * \param out_regionA location to which to write the first half
     * \param out_regionB location to which to write the second half
     * \param out_p location to which to write the position of the point
     *              on the curve in the middle (with repsect to time) of
     *              in_region
     */
    virtual
    void
    tessellate(reference_counted_ptr<tessellated_region> in_region,
               reference_counted_ptr<tessellated_region> *out_regionA,
               reference_counted_ptr<tessellated_region> *out_regionB,
               vec2 *out_p) const = 0;

    /*!
     * To be implemented by a derived class to return a reasonable
     * lower bound on the needed number of times the edge should be
     * cut in half in order to capture its shape.
     */
    virtual
    unsigned int
    minimum_tessellation_recursion(void) const = 0;
  };

  /*!
   * \brief
   * Derived class of interpolator_base to indicate a Bezier curve.
   * Supports Bezier curves of _any_ degree.
   */
  class bezier:public interpolator_generic
  {
  public:
    /*!
     * Ctor. One control point, thus interpolation is a quadratic curve.
     * \param start start of curve
     * \param ct control point
     * \param end end of curve
     * \param tp nature the edge represented by this interpolator_base
     */
    bezier(const reference_counted_ptr<const interpolator_base> &start,
           const vec2 &ct, const vec2 &end, enum PathEnums::edge_type_t tp);

    /*!
     * Ctor. Two control points, thus interpolation is a cubic curve.
     * \param start start of curve
     * \param ct1 1st control point
     * \param ct2 2nd control point
     * \param end end point of curve
     * \param tp nature the edge represented by this interpolator_base
     */
    bezier(const reference_counted_ptr<const interpolator_base> &start,
           const vec2 &ct1, const vec2 &ct2, const vec2 &end,
           enum PathEnums::edge_type_t tp);

    /*!
     * Ctor. Iterator range defines the control points of the bezier curve.
     * \param start start of curve
     * \param control_pts control points
     * \param end end point of curve
     * \param tp nature the edge represented by this interpolator_base
     */
    bezier(const reference_counted_ptr<const interpolator_base> &start,
           c_array<const vec2> control_pts, const vec2 &end,
           enum PathEnums::edge_type_t tp);

    virtual
    ~bezier();

    virtual
    bool
    is_flat(void) const;

    virtual
    void
    tessellate(reference_counted_ptr<tessellated_region> in_region,
               reference_counted_ptr<tessellated_region> *out_regionA,
               reference_counted_ptr<tessellated_region> *out_regionB,
               vec2 *out_p) const;
    virtual
    void
    approximate_bounding_box(vec2 *out_min_bb, vec2 *out_max_bb) const;

    virtual
    interpolator_base*
    deep_copy(const reference_counted_ptr<const interpolator_base> &prev) const;

    virtual
    unsigned int
    minimum_tessellation_recursion(void) const;

  private:
    bezier(const bezier &q,
           const reference_counted_ptr<const interpolator_base> &prev);

    void *m_d;
  };

  /*!
   * \brief
   * An arc is for connecting one point to the next via an
   * arc of a circle.
   */
  class arc:public interpolator_base
  {
  public:
    /*!
     * Ctor.
     * \param start start of curve
     * \param angle The angle of the arc in radians, the value must not
     *              be a multiple of 2*M_PI. Assuming a coordinate system
     *              where y-increases vertically and x-increases to the right,
     *              a positive value indicates to have the arc go counter-clockwise,
     *              a negative angle for the arc to go clockwise.
     * \param end end of curve
     * \param tp nature the edge represented by this interpolator_base
     */
    arc(const reference_counted_ptr<const interpolator_base> &start,
        float angle, const vec2 &end, enum PathEnums::edge_type_t tp);

    ~arc();

    virtual
    bool
    is_flat(void) const;

    virtual
    void
    approximate_bounding_box(vec2 *out_min_bb, vec2 *out_max_bb) const;

    virtual
    interpolator_base*
    deep_copy(const reference_counted_ptr<const interpolator_base> &prev) const;

    virtual
    reference_counted_ptr<tessellation_state>
    produce_tessellation(const TessellatedPath::TessellationParams &tess_params,
                         TessellatedPath::SegmentStorage *out_data,
                         float *out_max_distance) const;

  private:
    arc(const arc &q, const reference_counted_ptr<const interpolator_base> &prev);

    void *m_d;
  };

  /*!
   * Ctor.
   */
  explicit
  PathContour(void);

  ~PathContour();

  /*!
   * Start the PathContour, may only be called once in the lifetime
   * of a PathContour() and must be called before adding points
   * (to_point()), adding control points (add_control_point()),
   * adding arcs (to_arc()), adding generic interpolator (
   * to_generic()) or ending the contour (end(), end_generic()).
   */
  void
  start(const vec2 &pt);

  /*!
   * End the current edge.
   * \param pt point location of end of edge (and thus start of new edge)
   * \param etp the edge type of the new edge made; if this is the first
   *            edge of the contour, the value of etp is ignored and the
   *            value \ref PathEnums::starts_new_edge is used.
   */
  void
  to_point(const vec2 &pt, enum PathEnums::edge_type_t etp);

  /*!
   * Add a control point. Will fail if end() was called
   * \param pt location of new control point
   */
  void
  add_control_point(const vec2 &pt);

  /*!
   * Clear any current control points.
   */
  void
  clear_control_points(void);

  /*!
   * Will fail if end() was called of if add_control_point() has been
   * called more recently than to_point() or if interpolator_base::prev_interpolator()
   * of the passed interpolator_base is not the same as prev_interpolator().
   * \param p interpolator describing edge
   */
  void
  to_generic(const reference_counted_ptr<const interpolator_base> &p);

  /*!
   * Will fail if end() was called of if add_control_point() has been
   * called more recently than to_point().
   * \param angle angle of arc in radians
   * \param pt point where arc ends (and next edge starts)
   * \param etp the edge type of the new edge made; if this is the first
   *            edge of the contour, the value of etp is ignored and the
   *            value \ref PathEnums::starts_new_edge is used.
   */
  void
  to_arc(float angle, const vec2 &pt, enum PathEnums::edge_type_t etp);

  /*!
   * Ends with the passed interpolator_base. The interpolator
   * must interpolate to the start point of the PathContour
   * \param h interpolator describing the closing edge
   */
  void
  end_generic(reference_counted_ptr<const interpolator_base> h);

  /*!
   * Ends with the Bezier curve defined by the current
   * control points added by add_control_point().
   * \param etp the edge type of the new edge made.
   */
  void
  end(enum PathEnums::edge_type_t etp);

  /*!
   * Ends with an arc.
   * \param angle angle of arc in radians
   * \param etp the edge type of the new edge made.
   */
  void
  end_arc(float angle, enum PathEnums::edge_type_t etp);

  /*!
   * Returns the last interpolator added to this PathContour.
   * You MUST use this interpolator in the ctor of
   * interpolator_base for interpolator passed to
   * to_generic() and end_generic().
   */
  const reference_counted_ptr<const interpolator_base>&
  prev_interpolator(void);

  /*!
   * Returns true if the PathContour has ended
   */
  bool
  ended(void) const;

  /*!
   * Return the I'th point of this PathContour.
   * For I = 0, returns the value passed to start().
   * \param I index of point.
   */
  const vec2&
  point(unsigned int I) const;

  /*!
   * Returns the number of points of this PathContour.
   */
  unsigned int
  number_points(void) const;

  /*!
   * Returns the interpolator of this PathContour
   * that interpolates from the I'th point to the
   * (I+1)'th point. If I == number_points() - 1,
   * then returns the interpolator from the last
   * point to the first point.
   */
  const reference_counted_ptr<const interpolator_base>&
  interpolator(unsigned int I) const;

  /*!
   * Returns an approximation of the bounding box for
   * this PathContour WITHOUT relying on tessellating
   * the \ref interpolator_base objects of this \ref
   * PathContour IF this PathCountour is ended().
   * Returns true if this is ended() and writes the
   * values, otherwise returns false and does not write
   * any value.
   * \param out_min_bb (output) location to which to write the min-x and min-y
   *                            coordinates of the approximate bounding box.
   * \param out_max_bb (output) location to which to write the max-x and max-y
   *                            coordinates of the approximate bounding box.
   */
  bool
  approximate_bounding_box(vec2 *out_min_bb, vec2 *out_max_bb) const;

  /*!
   * Returns true if each interpolator of the PathContour is
   * flat.
   */
  bool
  is_flat(void) const;

  /*!
   * Create a deep copy of this PathContour.
   */
  PathContour*
  deep_copy(void);

private:
  void *m_d;
};

/*!
 * \brief
 * A Path represents a collection of PathContour
 * objects.
 *
 * To end a contour in a Path (see
 * \ref end_contour_quadratic(), \ref
 * end_contour_arc(), \ref end_contour_cubic(),
 * \ref end_contour_custom(), \ref contour_end_arc
 * and \ref contour_end) means to specify
 * the edge from the last point of the current
 * contour to the first point of it.
 */
class Path
{
public:
  /*!
   * \brief
   * Class that wraps a vec2 to mark a point
   * as a control point for a Bezier curve
   */
  class control_point
  {
  public:
    /*!
     * Ctor
     * \param pt value to which to set \ref m_location
     */
    explicit
    control_point(const vec2 &pt):
      m_location(pt)
    {}

    /*!
     * Ctor
     * \param x value to which to set m_location.x()
     * \param y value to which to set m_location.y()
     */
    control_point(float x, float y):
      m_location(x,y)
    {}

    /*!
     * Position of control point
     */
    vec2 m_location;
  };

  /*!
   * \brief
   * Wraps the data to specify an arc
   */
  class arc
  {
  public:
    /*!
     * Ctor
     * \param angle angle of arc in radians
     * \param pt point to which to arc
     */
    arc(float angle, const vec2 &pt):
      m_angle(angle), m_pt(pt)
    {}

    /*!
     * Angle of arc in radians
     */
    float m_angle;

    /*!
     * End point of arc
     */
    vec2 m_pt;
  };

  /*!
   * \brief
   * Tag class to mark the end of an contour
   */
  class contour_end
  {};

  /*!
   * \brief
   * Tag class to mark the end of an contour with an arc
   */
  class contour_end_arc
  {
  public:
    /*!
     * Ctor
     * \param angle angle of arc in radians
     */
    explicit
    contour_end_arc(float angle):
      m_angle(angle)
    {}

    /*!
     * Angle of arc in radians
     */
    float m_angle;
  };

  /*!
   * Ctor.
   */
  explicit
  Path(void);

  /*!
   * Copy ctor.
   * \param obj Path from which to copy path data
   */
  Path(const Path &obj);

  ~Path();

  /*!
   * Assignment operator
   * \param rhs value from which to assign.
   */
  const Path&
  operator=(const Path &rhs);

  /*!
   * Clear the path, i.e. remove all PathContour's from the
   * path
   */
  void
  clear(void);

  /*!
   * Swap contents of Path with another Path
   * \param obj Path with which to swap
   */
  void
  swap(Path &obj);

  /*!
   * Create an arc but specify the angle in degrees.
   * \param angle angle of arc in degrees
   * \param pt point to which to arc
   */
  static
  arc
  arc_degrees(float angle, const vec2 &pt)
  {
    return arc(angle*float(M_PI)/180.0f, pt);
  }

  /*!
   * Create an contour_end_arc but specify the angle in degrees.
   * \param angle angle or arc in degrees
   */
  static
  contour_end_arc
  contour_end_arc_degrees(float angle)
  {
    return contour_end_arc(angle*float(M_PI)/180.0f);
  }

  /*!
   * Operator overload to add a point of the current
   * contour in the Path.
   * \param pt point to add
   */
  Path&
  operator<<(const vec2 &pt);

  /*!
   * Operator overload to add a control point of the current
   * contour in the Path.
   * \param pt control point to add
   */
  Path&
  operator<<(const control_point &pt);

  /*!
   * Operator overload to add an arc to the current contour
   * in the Path.
   * \param a arc to add
   */
  Path&
  operator<<(const arc &a);

  /*!
   * Operator overload to end the current contour
   */
  Path&
  operator<<(contour_end);

  /*!
   * Operator overload to end the current contour
   * \param a specifies the angle of the arc for closing
   *          the current contour
   */
  Path&
  operator<<(contour_end_arc a);

  /*!
   * Operator overload to control the \ref edge_type_t
   * of the next edge made via operator overloads.
   * If no edge is yet present on the current contour, then
   * the value is ignored. The tag is reset back to \ref
   * PathEnums::starts_new_edge after an edge is added.
   * \param etp edge type
   */
  Path&
  operator<<(enum PathEnums::edge_type_t etp);

  /*!
   * Append a line to the current contour.
   * \param pt point to which the line goes
   * \param etp the edge type of the new line made; if this is the first
   *            edge of the current contour, the value of etp is ignored
   *            and the value \ref PathEnums::starts_new_edge is used.
   */
  Path&
  line_to(const vec2 &pt,
          enum PathEnums::edge_type_t etp = PathEnums::starts_new_edge);

  /*!
   * Append a quadratic Bezier curve to the current contour.
   * \param ct control point of the quadratic Bezier curve
   * \param pt point to which the quadratic Bezier curve goes
   * \param etp the edge type of the new quadratic made; if this is the first
   *            edge of the current contour, the value of etp is ignored
   *            and the value \ref PathEnums::starts_new_edge is used.
   */
  Path&
  quadratic_to(const vec2 &ct, const vec2 &pt,
               enum PathEnums::edge_type_t etp = PathEnums::starts_new_edge);

  /*!
   * Append a cubic Bezier curve to the current contour.
   * \param ct1 first control point of the cubic Bezier curve
   * \param ct2 second control point of the cubic Bezier curve
   * \param pt point to which the cubic Bezier curve goes
   * \param etp the edge type of the new cubic made; if this is the first
   *            edge of the current contour, the value of etp is ignored
   *            and the value \ref PathEnums::starts_new_edge is used.
   */
  Path&
  cubic_to(const vec2 &ct1, const vec2 &ct2, const vec2 &pt,
           enum PathEnums::edge_type_t etp = PathEnums::starts_new_edge);

  /*!
   * Append an arc curve to the current contour.
   * \param angle gives the angle of the arc in radians. For a coordinate system
   *              where y increases upwards and x increases to the right, a positive
   *              value indicates counter-clockwise and a negative value indicates
   *              clockwise
   * \param pt point to which the arc curve goes
   * \param etp the edge type of the new arc made; if this is the first
   *            edge of the current contour, the value of etp is ignored
   *            and the value \ref PathEnums::starts_new_edge is used.
   */
  Path&
  arc_to(float angle, const vec2 &pt,
         enum PathEnums::edge_type_t etp = PathEnums::starts_new_edge);

  /*!
   * Returns the last interpolator added to this the current
   * contour of this Path. When creating custom
   * interpolator to be added with custom_to(),
   * You MUST use this interpolator in the ctor of
   * interpolator_base.
   */
  const reference_counted_ptr<const PathContour::interpolator_base>&
  prev_interpolator(void);

  /*!
   * Add a custom interpolator. Use prev_interpolator()
   * to get the last interpolator of the current contour.
   */
  Path&
  custom_to(const reference_counted_ptr<const PathContour::interpolator_base> &p);

  /*!
   * End the current contour and begin a new contour
   * \param pt point at which the contour begins
   * \param etp the edge type of the closing edge made.
   */
  Path&
  move(const vec2 &pt,
       enum PathEnums::edge_type_t etp = PathEnums::starts_new_edge);

  /*!
   * End the current contour.
   * \param etp the edge type of the closing edge made.
   */
  Path&
  end_contour(enum PathEnums::edge_type_t etp = PathEnums::starts_new_edge);

  /*!
   * End the current contour in an arc and begin a new contour
   * \param angle gives the angle of the arc in radians. For a coordinate system
   *              where y increases upwards and x increases to the right, a positive
   *              value indicates counter-clockwise and a negative value indicates
   *              clockwise
   * \param pt point at which the contour begins
   * \param etp the edge type of the closing edge made.
   */
  Path&
  arc_move(float angle, const vec2 &pt,
           enum PathEnums::edge_type_t etp = PathEnums::starts_new_edge);

  /*!
   * End the current contour in an arc
   * \param angle gives the angle of the arc in radians. For a coordinate system
   *              where y increases upwards and x increases to the right, a positive
   *              value indicates counter-clockwise and a negative value indicates
   *              clockwise
   * \param etp the edge type of the closing edge made.
   */
  Path&
  end_contour_arc(float angle,
                  enum PathEnums::edge_type_t etp = PathEnums::starts_new_edge);

  /*!
   * End the current contour in a quadratic Bezier curve and begin a new contour
   * \param ct control point of the quadratic Bezier curve
   * \param pt point at which the contour begins
   * \param etp the edge type of the closing edge made.
   */
  Path&
  quadratic_move(const vec2 &ct, const vec2 &pt,
                 enum PathEnums::edge_type_t etp = PathEnums::starts_new_edge);

  /*!
   * End the current contour in a quadratic Bezier curve
   * \param ct control point of the quadratic Bezier curve
   * \param etp the edge type of the closing edge made.
   */
  Path&
  end_contour_quadratic(const vec2 &ct,
                        enum PathEnums::edge_type_t etp = PathEnums::starts_new_edge);

  /*!
   * End the current contour in a cubic Bezier curve and begin a new contour
   * \param ct1 first control point of the cubic Bezier curve
   * \param ct2 second control point of the cubic Bezier curve
   * \param pt point at which the contour begins
   * \param etp the edge type of the closing edge made.
   */
  Path&
  cubic_move(const vec2 &ct1, const vec2 &ct2, const vec2 &pt,
             enum PathEnums::edge_type_t etp = PathEnums::starts_new_edge);

  /*!
   * End the current contour in a cubic Bezier curve
   * \param ct1 first control point of the cubic Bezier curve
   * \param ct2 second control point of the cubic Bezier curve
   * \param etp the edge type of the closing edge made.
   */
  Path&
  end_contour_cubic(const vec2 &ct1, const vec2 &ct2,
                    enum PathEnums::edge_type_t etp = PathEnums::starts_new_edge);

  /*!
   * Use a custom interpolator to end the current contour and begin a new contour
   * Use prev_interpolator() to get the last interpolator
   * of the current contour.
   * \param p custom interpolator
   * \param pt point at which the contour begins
   */
  Path&
  custom_move(const reference_counted_ptr<const PathContour::interpolator_base> &p, const vec2 &pt);

  /*!
   * Use a custom interpolator to end the current contour
   * Use prev_interpolator() to get the last interpolator
   * of the current contour.
   * \param p custom interpolator
   */
  Path&
  end_contour_custom(const reference_counted_ptr<const PathContour::interpolator_base> &p);

  /*!
   * Adds a PathContour to this Path. PathContour is only added
   * if the contour is ended (i.e. PathContour::ended() returns
   * true). A fixed, ended PathContour can be used by multiple
   * Path's at the same time.
   * \param contour PathContour to add to the Path
   */
  Path&
  add_contour(const reference_counted_ptr<const PathContour> &contour);

  /*!
   * Add all the ended PathContour objects of a Path into this Path.
   * \param path Path to add
   */
  Path&
  add_contours(const Path &path);

  /*!
   * Returns the number of contours of the Path.
   */
  unsigned int
  number_contours(void) const;

  /*!
   * Returns the named contour
   * \param i index of contour to fetch (0 <= i < number_contours())
   */
  reference_counted_ptr<const PathContour>
  contour(unsigned int i) const;

  /*!
   * Returns true if each PathContour of the Path is flat.
   */
  bool
  is_flat(void) const;

  /*!
   * Returns an approximation of the bounding box for
   * the PathContour::ended() PathContour obejcts of
   * this Path WITHOUT relying on tessellating
   * (or for that matter creating a TessellatedPath)
   * this Path. Returns true if atleast one PathContour
   * of this Path is PathContour::ended() and writes the
   * values, otherwise returns false and does not write
   * any value.
   * \param out_min_bb (output) location to which to write the min-x and min-y
   *                            coordinates of the approximate bounding box.
   * \param out_max_bb (output) location to which to write the max-x and max-y
   *                            coordinates of the approximate bounding box.
   */
  bool
  approximate_bounding_box(vec2 *out_min_bb, vec2 *out_max_bb) const;

  /*!
   * Return the tessellation of this Path at a specific
   * level of detail. The TessellatedPath is constructed
   * lazily. Additionally, if this Path changes its geometry,
   * then a new TessellatedPath will be contructed on the
   * next call to tessellation().
   * \param thresh the returned tessellated path will be so that
   *               TessellatedPath::max_distance() is no more than
   *               thresh. A non-positive value will return the
   *               lowest level of detail tessellation.
   */
  const reference_counted_ptr<const TessellatedPath>&
  tessellation(float thresh) const;

  /*!
   * Provided as a conveniance, returns the starting point tessellation.
   * Equivalent to
   * \code
   * tessellation(-1.0f)
   * \endcode
   */
  const reference_counted_ptr<const TessellatedPath>&
  tessellation(void) const;

  /*!
   * Return the arc-tessellation of this Path at a specific
   * level of detail. The TessellatedPath is constructed
   * lazily. Additionally, if this Path changes its geometry,
   * then a new TessellatedPath will be contructed on the
   * next call to arc_tessellation().
   * \param max_distance the returned tessellated path will be so that
   *                     TessellatedPath::effective_max_distance()
   *                     is no more than thresh. A non-positive value
   *                     will return the starting point tessellation.
   */
  const reference_counted_ptr<const TessellatedPath>&
  arc_tessellation(float max_distance) const;

  /*!
   * Provided as a conveniance, returns the starting point tessellation.
   * Equivalent to
   * \code
   * arc_tessellation(-1.0f)
   * \endcode
   */
  const reference_counted_ptr<const TessellatedPath>&
  arc_tessellation(void) const;

private:
  void *m_d;
};

/*! @} */

}
