# Begin standard header
sp 		:= $(sp).x
dirstack_$(sp)	:= $(d)
d		:= $(dir)
# End standard header

LIBRARY_SOURCES += $(call filelist, shader_source.cpp\
	painter_item_shader_glsl.cpp painter_blend_shader_glsl.cpp)


# Begin standard footer
d		:= $(dirstack_$(sp))
sp		:= $(basename $(sp))
# End standard footer
