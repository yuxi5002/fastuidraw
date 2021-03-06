/*!
 * \file uber_shader_builder.cpp
 * \brief file uber_shader_builder.cpp
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

#include <sstream>
#include <iomanip>

#include "uber_shader_builder.hpp"
#include "../../private/util_private.hpp"
#include "../../private/util_private_ostream.hpp"

namespace
{
  void
  stream_varyings_as_local_variables_array(fastuidraw::glsl::ShaderSource &vert,
                                           fastuidraw::c_array<const fastuidraw::c_string> p,
                                           fastuidraw::c_string type)
  {
    for(fastuidraw::c_string str : p)
      {
        vert << type << " " << str << ";\n";
      }
  }

  std::string
  make_name(fastuidraw::c_string name,
            unsigned int idx)
  {
    std::ostringstream str;

    str << name << idx;
    return str.str();
  }

  class pre_stream_varyings
  {
  public:
    pre_stream_varyings(const fastuidraw::glsl::detail::UberShaderVaryings &src,
                        const fastuidraw::glsl::detail::AliasVaryingLocation &datum):
      m_src(src),
      m_datum(datum)
    {}

    void
    operator()(fastuidraw::glsl::ShaderSource &dst,
               const fastuidraw::reference_counted_ptr<fastuidraw::glsl::PainterItemShaderGLSL> &sh) const
    {
      m_src.stream_alias_varyings(dst, sh->varyings(), true, m_datum);
    }

  private:
    const fastuidraw::glsl::detail::UberShaderVaryings &m_src;
    const fastuidraw::glsl::detail::AliasVaryingLocation &m_datum;
  };

  class post_stream_varyings
  {
  public:
    post_stream_varyings(const fastuidraw::glsl::detail::UberShaderVaryings &src,
                         const fastuidraw::glsl::detail::AliasVaryingLocation &datum):
      m_src(src),
      m_datum(datum)
    {}

    void
    operator()(fastuidraw::glsl::ShaderSource &dst,
               const fastuidraw::reference_counted_ptr<fastuidraw::glsl::PainterItemShaderGLSL> &sh) const
    {
      m_src.stream_alias_varyings(dst, sh->varyings(), false, m_datum);
    }

  private:
    const fastuidraw::glsl::detail::UberShaderVaryings &m_src;
    const fastuidraw::glsl::detail::AliasVaryingLocation &m_datum;
  };

  void
  add_macro_requirement(fastuidraw::glsl::ShaderSource &dst,
                        bool should_be_defined,
                        fastuidraw::c_string macro,
                        fastuidraw::c_string error_message)
  {
    fastuidraw::c_string not_cnd, msg;

    not_cnd = (should_be_defined) ? "!defined" : "defined";
    msg = (should_be_defined) ? "" : "not ";
    dst << "#if " << not_cnd << "(" << macro << ")\n"
        << "#error \"" << error_message << ": "
        << macro << " should " << msg << "be defined\"\n"
        << "#endif\n";
  }

  void
  add_macro_requirement(fastuidraw::glsl::ShaderSource &dst,
                        fastuidraw::c_string macro1,
                        fastuidraw::c_string macro2,
                        fastuidraw::c_string error_message)
  {
    dst << "#if (!defined(" << macro1 << ") && !defined(" << macro2 << ")) "
        << " || (defined(" << macro1 << ") && defined(" << macro2 << "))\n"
        << "#error \"" << error_message << ": exactly one of "
        << macro1 << " or " << macro2 << " should be defined\"\n"
        << "#endif\n";
  }

  template<typename T>
  class UberShaderStreamer
  {
  public:
    typedef fastuidraw::glsl::ShaderSource ShaderSource;
    typedef fastuidraw::reference_counted_ptr<T> ref_type;
    typedef fastuidraw::c_array<const ref_type> array_type;
    typedef const ShaderSource& (T::*get_src_type)(void) const;

    static
    void
    stream_nothing(ShaderSource &, const ref_type &)
    {}

    template<typename pre_stream_type,
             typename post_stream_type>
    static
    void
    stream_uber(bool use_switch, ShaderSource &dst, array_type shaders,
                get_src_type get_src,
                pre_stream_type pre_stream,
                post_stream_type post_stream,
                const std::string &return_type,
                const std::string &uber_func_with_args,
                const std::string &shader_main,
                const std::string &shader_args, //of the form ", arg1, arg2,..,argN" or empty string
                const std::string &shader_id);

    static
    void
    stream_uber(bool use_switch, ShaderSource &dst, array_type shaders,
                get_src_type get_src,
                const std::string &return_type,
                const std::string &uber_func_with_args,
                const std::string &shader_main,
                const std::string &shader_args,
                const std::string &shader_id)
    {
      stream_uber(use_switch, dst, shaders, get_src,
                  &stream_nothing, &stream_nothing,
                  return_type, uber_func_with_args,
                  shader_main, shader_args, shader_id);
    }
  private:
    static
    void
    stream_source(ShaderSource &dst, const std::string &prefix,
                  const ShaderSource &shader);
  };

}

template<typename T>
void
UberShaderStreamer<T>::
stream_source(ShaderSource &dst, const std::string &prefix,
              const ShaderSource &shader)
{
  /* This terribly hack is because GLES specfication mandates
   * for GLSL in GLES to not support token pasting (##) in the
   * pre-processor. Many GLES drivers support it anyways, but
   * Mesa does not. Sighs. So we emulate the token pasting
   * for the FASTUIDRAW_LOCAL() macro.
   */
  using namespace fastuidraw;
  std::string src, needle;
  std::string::size_type pos, last_pos;

  needle = "FASTUIDRAW_LOCAL";
  src = shader.assembled_code(true);

  /* HUNT for FASTUIDRAW_LOCAL(X) anywhere in the code and expand
   * it. NOTE: this is NOT robust at all as it is not a real
   * pre-processor, just a hack. It will fail if the macro
   * invocation is spread across multiple lines or if the agument
   * was a macro itself that needs to expanded by the pre-processor.
   */
  for (last_pos = 0, pos = src.find(needle); pos != std::string::npos; last_pos = pos + 1, pos = src.find(needle, pos + 1))
    {
      std::string::size_type open_pos, close_pos;

      /* stream up to pos */
      if (pos > last_pos)
        {
          dst << src.substr(last_pos, pos - last_pos);
        }

      /* find the first open and close-parentesis pair after pos. */
      open_pos = src.find('(', pos);
      close_pos = src.find(')', pos);

      if (open_pos != std::string::npos
          && close_pos != std::string::npos
          && open_pos < close_pos)
        {
          std::string tmp;
          tmp = src.substr(open_pos + 1, close_pos - open_pos - 1);
          // trim the string of white spaces.
          tmp.erase(0, tmp.find_first_not_of(" \t"));
          tmp.erase(tmp.find_last_not_of(" \t") + 1);
          dst << prefix << tmp;
          pos = close_pos;
        }
    }

  dst << src.substr(last_pos)
      << "\n";
}

template<typename T>
template<typename pre_stream_type,
         typename post_stream_type>
void
UberShaderStreamer<T>::
stream_uber(bool use_switch, ShaderSource &dst, array_type shaders,
            get_src_type get_src,
            pre_stream_type pre_stream,
            post_stream_type post_stream,
            const std::string &return_type,
            const std::string &uber_func_with_args,
            const std::string &shader_main,
            const std::string &shader_args, //of the form ", arg1, arg2,..,argN" or empty string
            const std::string &shader_id)
{
  /* first stream all of the item_shaders with predefined macros. */
  for(const auto &sh : shaders)
    {
      std::ostringstream str, prefix;

      dst << "\n/////////////////////////////////////////\n"
          << "// Start Shader #" << sh->ID() << "\n";

      str << shader_main << sh->ID();
      prefix << shader_main << "_local_" << sh->ID() << "_";

      pre_stream(dst, sh);
      dst.add_macro(shader_main.c_str(), str.str().c_str());
      stream_source(dst, prefix.str(), (sh.get()->*get_src)());
      dst.remove_macro(shader_main.c_str());
      post_stream(dst, sh);
    }

  bool has_sub_shaders(false), has_return_value(return_type != "void");
  std::string tab;

  dst << return_type << "\n"
      << uber_func_with_args << "\n"
      << "{\n";

  if (has_return_value)
    {
      dst << "    " << return_type << " p;\n";
    }

  for(const auto &sh : shaders)
    {
      if (sh->number_sub_shaders() > 1)
        {
          unsigned int start, end;
          start = sh->ID();
          end = start + sh->number_sub_shaders();
          if (has_sub_shaders)
            {
              dst << "    else ";
            }
          else
            {
              dst << "    ";
            }

          dst << "if (" << shader_id << " >= uint(" << start
              << ") && " << shader_id << " < uint(" << end << "))\n"
              << "    {\n"
              << "        ";
          if (has_return_value)
            {
              dst << "p = ";
            }
          dst << shader_main << sh->ID()
              << "(" << shader_id << " - uint(" << start << ")" << shader_args << ");\n"
              << "    }\n";
          has_sub_shaders = true;
        }
    }

  if (has_sub_shaders && use_switch)
    {
      dst << "    else\n"
          << "    {\n";
      tab = "        ";
    }
  else
    {
      tab = "    ";
    }

  if (use_switch)
    {
      dst << tab << "switch(" << shader_id << ")\n"
          << tab << "{\n";
    }

  bool first_entry(true);
  for(const auto &sh : shaders)
    {
      if (sh->number_sub_shaders() == 1)
        {
          if (use_switch)
            {
              dst << tab << "case uint(" << sh->ID() << "):\n"
                  << tab << "    {\n"
                  << tab << "        ";
            }
          else
            {
              if (!first_entry)
                {
                  dst << tab << "else if";
                }
              else
                {
                  dst << tab << "if";
                }
              dst << "(" << shader_id << " == uint(" << sh->ID() << "))\n"
                  << tab << "{\n"
                  << tab << "    ";
            }

          if (has_return_value)
            {
              dst << "p = ";
            }

          dst << shader_main << sh->ID()
              << "(uint(0)" << shader_args << ");\n";

          if (use_switch)
            {
              dst << tab << "    }\n"
                  << tab << "    break;\n\n";
            }
          else
            {
              dst << tab << "}\n";
            }
          first_entry = false;
        }
    }

  if (use_switch)
    {
      dst << tab << "}\n";
    }

  if (has_sub_shaders && use_switch)
    {
      dst << "    }\n";
    }

  if (has_return_value)
    {
      dst << "    return p;\n";
    }

  dst << "}\n";
}


namespace fastuidraw { namespace glsl { namespace detail {

//////////////////////////
// UberShaderVaryings methods
void
UberShaderVaryings::
add_varyings(c_string label,
             size_t uint_count,
             size_t int_count,
             c_array<const size_t> float_counts,
             AliasVaryingLocation *datum)
{
  c_string uint_labels[]=
    {
      "uint",
      "uvec2",
      "uvec3",
      "uvec4",
    };

  c_string int_labels[]=
    {
      "int",
      "ivec2",
      "ivec3",
      "ivec4",
    };

  c_string float_labels[]=
    {
      "float",
      "vec2",
      "vec3",
      "vec4",
    };

  datum->m_uint_varying_start = add_varyings_impl_type(m_uint_varyings, uint_count,
                                                       "flat", uint_labels,
                                                       "fastuidraw_uint_varying", true);

  datum->m_int_varying_start = add_varyings_impl_type(m_int_varyings, int_count,
                                                      "flat", int_labels,
                                                      "fastuidraw_int_varying", true);

  datum->m_float_varying_start[varying_list::interpolation_smooth] =
    add_varyings_impl_type(m_float_varyings[varying_list::interpolation_smooth],
                           float_counts[varying_list::interpolation_smooth],
                           "", float_labels,
                           "fastuidraw_float_smooth_varying",
                           false);

  datum->m_float_varying_start[varying_list::interpolation_flat] =
    add_varyings_impl_type(m_float_varyings[varying_list::interpolation_flat],
                           float_counts[varying_list::interpolation_flat],
                           "flat", float_labels,
                           "fastuidraw_float_flat_varying",
                           true);

  datum->m_float_varying_start[varying_list::interpolation_noperspective] =
    add_varyings_impl_type(m_float_varyings[varying_list::interpolation_noperspective],
                           float_counts[varying_list::interpolation_noperspective],
                           "noperspective", float_labels,
                           "fastuidraw_float_noperspective_varying",
                           false);

  datum->m_label = label;
}

uvec2
UberShaderVaryings::
add_varyings_impl_type(std::vector<per_varying> &varyings,
                       unsigned int cnt,
                       c_string qualifier, c_string types[],
                       c_string name, bool is_flat)
{
  uvec2 return_value;

  /* first add to the back of varying up to 4-components */
  if (!varyings.empty() && varyings.back().m_num_components < 4)
    {
      unsigned int add_to_back, space_left;

      return_value.x() = varyings.size() - 1;
      return_value.y() = varyings.back().m_num_components;

      space_left = 4u - varyings.back().m_num_components;
      add_to_back = t_min(space_left, cnt);

      varyings.back().m_num_components += add_to_back;
      varyings.back().m_type = types[varyings.back().m_num_components - 1];

      cnt -= add_to_back;
    }
  else
    {
      return_value.x() = varyings.size();
      return_value.y() = 0u;
    }

  if (cnt == 0)
    {
      return return_value;
    }

  per_varying V;
  unsigned int num_vec4(cnt / 4);
  unsigned int remaining(cnt % 4);

  V.m_is_flat = is_flat;
  V.m_qualifier = qualifier;
  for (unsigned int i = 0; i < num_vec4; ++i)
    {
      V.m_name = make_name(name, varyings.size());
      V.m_num_components = 4;
      V.m_type = types[V.m_num_components - 1];
      varyings.push_back(V);
    }

  if (remaining > 0)
    {
      V.m_name = make_name(name, varyings.size());
      V.m_num_components = remaining;
      V.m_type = types[V.m_num_components - 1];
      varyings.push_back(V);
    }

  return return_value;
}

void
UberShaderVaryings::
declare_varyings(std::ostringstream &str,
                 c_string varying_qualifier,
                 c_string interface_name,
                 c_string instance_name) const
{
  unsigned int cnt(0);
  if (interface_name)
    {
      str << varying_qualifier << " " << interface_name << "\n{\n";
      varying_qualifier = "";
    }

  declare_varyings_impl(str, m_uint_varyings, varying_qualifier, cnt);
  declare_varyings_impl(str, m_int_varyings, varying_qualifier, cnt);
  for (unsigned int i = 0; i < varying_list::interpolation_number_types; ++i)
    {
      declare_varyings_impl(str, m_float_varyings[i], varying_qualifier, cnt);
    }

  if (interface_name)
    {
      str << "}";
      if (instance_name)
        {
          str << " " << instance_name;
        }
      str << ";\n";
    }
}

void
UberShaderVaryings::
declare_varyings_impl(std::ostringstream &str,
                      const std::vector<per_varying> &varyings,
                      c_string varying_qualifier,
                      unsigned int &slot) const
{
  for (const per_varying &V : varyings)
    {
      str << "FASTUIDRAW_LAYOUT_VARYING(" << slot << ") "
          << V.m_qualifier << " " << varying_qualifier << " " << V.m_type
          << " " << V.m_name << ";\n";
      ++slot;
    }
}

void
UberShaderVaryings::
stream_alias_varyings_impl(const std::vector<per_varying> &varyings_to_use,
                           ShaderSource &shader,
                           c_array<const c_string> p,
                           bool add_aliases, uvec2 start) const
{
  if (add_aliases)
    {
      fastuidraw::c_string ext = "xyzw";
      for (unsigned int i = 0; i < p.size(); ++i, ++start.y())
        {
          if (start.y() == 4)
            {
              ++start.x();
              start.y() = 0;
            }

          FASTUIDRAWassert(start.x() < varyings_to_use.size());
          FASTUIDRAWassert(start.y() < 4u);

          const per_varying &V(varyings_to_use[start.x()]);
          std::ostringstream str;

          str << V.m_name;
          if (V.m_num_components != 1)
            {
              str << "." << ext[start.y()];
            }
          shader.add_macro(p[i], str.str().c_str());
        }
    }
  else
    {
      for (unsigned int i = 0; i < p.size(); ++i)
        {
          shader.remove_macro(p[i]);
        }
    }
}

void
UberShaderVaryings::
stream_alias_varyings(ShaderSource &shader, const varying_list &p,
                      bool add_aliases,
                      const AliasVaryingLocation &datum) const
{
  shader << "//////////////////////////////////////////////////\n"
         << "// Stream variable aliases for: " << datum.m_label
         << " u@" << datum.m_uint_varying_start
         << " i@" << datum.m_int_varying_start
         << " f@" << datum.m_float_varying_start
         << "\n";

  stream_alias_varyings_impl(m_uint_varyings, shader, p.uints(),
                             add_aliases, datum.m_uint_varying_start);

  stream_alias_varyings_impl(m_int_varyings, shader, p.ints(),
                             add_aliases, datum.m_int_varying_start);
  for (unsigned int i = 0; i < varying_list::interpolation_number_types; ++i)
    {
      enum varying_list::interpolation_qualifier_t q;
      q = static_cast<enum varying_list::interpolation_qualifier_t>(i);

      stream_alias_varyings_impl(m_float_varyings[i], shader, p.floats(q),
                                 add_aliases, datum.m_float_varying_start[i]);
    }
}

/////////////////////
// non-class methods
void
stream_as_local_variables(ShaderSource &shader, const varying_list &p)
{
  stream_varyings_as_local_variables_array(shader, p.uints(), "uint");
  stream_varyings_as_local_variables_array(shader, p.ints(), "uint");

  for(unsigned int i = 0; i < varying_list::interpolation_number_types; ++i)
    {
      enum varying_list::interpolation_qualifier_t q;
      q = static_cast<enum varying_list::interpolation_qualifier_t>(i);
      stream_varyings_as_local_variables_array(shader, p.floats(q), "float");
    }
}

void
stream_uber_vert_shader(bool use_switch,
                        ShaderSource &vert,
                        c_array<const reference_counted_ptr<PainterItemShaderGLSL> > item_shaders,
                        const UberShaderVaryings &declare_varyings,
                        const AliasVaryingLocation &datum)
{
  UberShaderStreamer<PainterItemShaderGLSL>::stream_uber(use_switch, vert, item_shaders,
                                                         &PainterItemShaderGLSL::vertex_src,
                                                         pre_stream_varyings(declare_varyings, datum),
                                                         post_stream_varyings(declare_varyings, datum),
                                                         "vec4", "fastuidraw_run_vert_shader(in fastuidraw_shader_header h, out int add_z)",
                                                         "fastuidraw_gl_vert_main",
                                                         ", fastuidraw_primary_attribute, fastuidraw_secondary_attribute, "
                                                         "fastuidraw_uint_attribute, h.item_shader_data_location, add_z",
                                                         "h.item_shader");
}

void
stream_uber_frag_shader(bool use_switch,
                        ShaderSource &frag,
                        c_array<const reference_counted_ptr<PainterItemShaderGLSL> > item_shaders,
                        const UberShaderVaryings &declare_varyings,
                        const AliasVaryingLocation &datum)
{
  UberShaderStreamer<PainterItemShaderGLSL>::stream_uber(use_switch, frag, item_shaders,
                                                         &PainterItemShaderGLSL::fragment_src,
                                                         pre_stream_varyings(declare_varyings, datum),
                                                         post_stream_varyings(declare_varyings, datum),
                                                         "vec4",
                                                         "fastuidraw_run_frag_shader(in uint frag_shader, "
                                                         "in uint frag_shader_data_location)",
                                                         "fastuidraw_gl_frag_main", ", frag_shader_data_location",
                                                         "frag_shader");
}

void
stream_uber_blend_shader(bool use_switch,
                         ShaderSource &frag,
                         c_array<const reference_counted_ptr<PainterBlendShaderGLSL> > shaders,
                         enum PainterBlendShader::shader_type tp)
{
  std::string sub_func_name, func_name, sub_func_args;

  switch(tp)
    {
    default:
      FASTUIDRAWassert("Unknown blend_code_type!");
      //fall through
    case PainterBlendShader::single_src:
      func_name = "fastuidraw_run_blend_shader(in uint blend_shader, in uint blend_shader_data_location, in vec4 in_src, out vec4 out_src)";
      sub_func_name = "fastuidraw_gl_compute_blend_value";
      sub_func_args = ", blend_shader_data_location, in_src, out_src";
      add_macro_requirement(frag, true, "FASTUIDRAW_PAINTER_BLEND_SINGLE_SRC_BLEND", "Mismatch macros determining blend shader type!");
      add_macro_requirement(frag, false, "FASTUIDRAW_PAINTER_BLEND_DUAL_SRC_BLEND", "Mismatch macros determining blend shader type!");
      add_macro_requirement(frag, false, "FASTUIDRAW_PAINTER_BLEND_FRAMEBUFFER_FETCH", "Mismatch macros determining blend shader type!");
      add_macro_requirement(frag, false, "FASTUIDRAW_PAINTER_BLEND_INTERLOCK", "Mismatch macros determining blend shader type!");
      break;

    case PainterBlendShader::dual_src:
      func_name = "fastuidraw_run_blend_shader(in uint blend_shader, in uint blend_shader_data_location, in vec4 color0, out vec4 src0, out vec4 src1)";
      sub_func_name = "fastuidraw_gl_compute_blend_factors";
      sub_func_args = ", blend_shader_data_location, color0, src0, src1";
      add_macro_requirement(frag, false, "FASTUIDRAW_PAINTER_BLEND_SINGLE_SRC_BLEND", "Mismatch macros determining blend shader type");
      add_macro_requirement(frag, true, "FASTUIDRAW_PAINTER_BLEND_DUAL_SRC_BLEND", "Mismatch macros determining blend shader type");
      add_macro_requirement(frag, false, "FASTUIDRAW_PAINTER_BLEND_FRAMEBUFFER_FETCH", "Mismatch macros determining blend shader type");
      add_macro_requirement(frag, false, "FASTUIDRAW_PAINTER_BLEND_INTERLOCK", "Mismatch macros determining blend shader type!");
      break;

    case PainterBlendShader::framebuffer_fetch:
      func_name = "fastuidraw_run_blend_shader(in uint blend_shader, in uint blend_shader_data_location, in vec4 in_src, in vec4 in_fb, out vec4 out_src)";
      sub_func_name = "fastuidraw_gl_compute_post_blended_value";
      sub_func_args = ", blend_shader_data_location, in_src, in_fb, out_src";
      add_macro_requirement(frag, false, "FASTUIDRAW_PAINTER_BLEND_SINGLE_SRC_BLEND", "Mismatch macros determining blend shader type");
      add_macro_requirement(frag, false, "FASTUIDRAW_PAINTER_BLEND_DUAL_SRC_BLEND", "Mismatch macros determining blend shader type");
      add_macro_requirement(frag,
                            "FASTUIDRAW_PAINTER_BLEND_FRAMEBUFFER_FETCH",
                            "FASTUIDRAW_PAINTER_BLEND_INTERLOCK",
                            "Mismatch macros determining blend shader type");
      break;
    }
  UberShaderStreamer<PainterBlendShaderGLSL>::stream_uber(use_switch, frag, shaders,
                                                          &PainterBlendShaderGLSL::blend_src,
                                                          "void", func_name,
                                                          sub_func_name, sub_func_args, "blend_shader");
}

}}}
