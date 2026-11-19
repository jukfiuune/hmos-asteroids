#version 450
// A triangle spinning about the center, kept round on a tall panel (aspect = width / height).
layout(push_constant) uniform PC { float t; float aspect; } pc;
layout(location = 0) out vec3 color;

void main()
{
    const vec3 colors[3] = vec3[](vec3(1, 0.2, 0.2), vec3(0.2, 1, 0.2), vec3(0.3, 0.4, 1));
    float a = pc.t + float(gl_VertexIndex) * 2.0943951;
    gl_Position = vec4(0.8 * sin(a), -0.8 * cos(a) * pc.aspect, 0, 1);
    color = colors[gl_VertexIndex];
}
