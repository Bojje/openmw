#version 460

// Matches MyGUI::Vertex exactly: 24 bytes, position / packed colour / texture coordinate, in that
// order. The colour sits between the position and the UV rather than after them, which is easy to
// get wrong and produces garbage geometry rather than a wrong colour.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragTexCoord;

void main()
{
    fragColor = inColor;
    fragTexCoord = inTexCoord;

    // MyGUI emits clip space directly. Its widgets already know the view size through
    // RenderTargetInfo::pixScaleX/pixScaleY, so a skin lays itself out and converts to normalised
    // device coordinates itself; there is no model, view or projection matrix anywhere in the
    // platform interface and none is wanted here.
    //
    // The convention it emits is OpenGL's, with Y up -- MyGUI::SubSkin negates the vertical when it
    // builds a quad, so the top of a widget lands at +1. Vulkan's Y points down, so the sign has to
    // come back off. Getting this wrong renders the whole interface upside down, which reads as a
    // layout bug rather than a coordinate one.
    //
    // Z is RenderTargetInfo::maximumDepth, which the render manager sets to 1. That is inside
    // Vulkan's [0, w] clip range as well as OpenGL's [-w, w], and the pipeline has depth testing off
    // in any case, so it is carried through untouched.
    gl_Position = vec4(inPosition.x, -inPosition.y, inPosition.z, 1.0);
}
