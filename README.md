# ImGuiColorTextEdit
Syntax highlighting text editor for ImGui, specialized for shader programming in GLSL and HLSL.

While it relies on Omar Cornut's https://github.com/ocornut/imgui, it does not follow the "pure" one widget - one function approach. Since the editor has to maintain a relatively complex and large internal state, it did not seem to be practical to try and enforce fully immediate mode. It stores its internal state in an object instance which is reused across frames.

The code is work in progress, please report if you find any issues.

# Main features
 - Approximates typical code editor look and feel (essential mouse/keyboard commands work - I mean, the commands _I_ normally use :))
 - Undo/Redo
 - UTF-8 support
 - Works with both fixed and variable-width fonts
 - Extensible syntax highlighting for multiple languages
 - Identifier declarations: a small piece of description can be associated with an identifier. The editor displays it in a tooltip when the mouse cursor is hovered over the identifier
 - Error markers: the user can specify a list of error messages together the line of occurence, the editor will highligh the lines with red backround and display error message in a tooltip when the mouse cursor is hovered over the line
 - Large files: there is no explicit limit set on file size or number of lines (below 2GB, performance is not affected when large files are loaded (except syntax coloring, see below)
 - Color palette support: you can switch between different color palettes, or even define your own
 - Whitespace indicators (TAB, space)
 
# Known issues
 - Syntax highligthing of most languages - except C/C++ - is based on std::regex, which is diasppointingly slow. Because of that, the highlighting process is amortized between multiple frames. C/C++ has a hand-written tokenizer which is much faster. 
