/* Goxel 3D voxels editor
 *
 * copyright (c) 2015 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.

 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

extern "C" {
#include "goxel.h"
}

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"

static inline ImVec4 IMHEXCOLOR(uint32_t v)
{
    uvec4b_t c = HEXCOLOR(v);
    return ImVec4(c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0);
}

namespace ImGui {
    bool GoxSelectable(const char *name, bool *v, int tex = 0, int icon = 0,
                       const char *tooltip = NULL, ImVec2 size = ImVec2(0, 0));
    bool GoxColorEdit(const char *name, uvec4b_t *color);
    bool GoxPaletteEntry(const uvec4b_t *color, uvec4b_t *target);
    bool GoxIsCharPressed(int c);
    bool GoxCollapsingHeader(const char *label, const char *str_id = NULL,
                             bool display_frame = true,
                             bool default_open = false);
    bool GoxAction(const char *id, const char *label, float size,
                   const char *sig, ...);

    bool GoxCheckbox(const char *id, const char *label);
    bool GoxMenuItem(const char *id, const char *label);
    bool GoxInputAngle(const char *id, float *v, int vmin, int vmax);
    bool GoxTab(const char *label, bool *v);
    bool GoxInputInt(const char *label, int *v, int step = 1,
                     float minv = -FLT_MAX, float maxv = FLT_MAX);
    bool GoxInputFloat(const char *label, float *v, float step = 0.1,
                       float minv = -FLT_MAX, float maxv = FLT_MAX,
                       const char *format = "%.1f");

    bool GoxInputQuat(const char *label, quat_t *q);

    void GoxGroupBegin(const char *label = NULL);
    void GoxGroupEnd(void);
};

static texture_t *g_tex_icons = NULL;

static const int MiB = 1 << 20;

static ImVec4 uvec4b_to_imvec4(uvec4b_t v)
{
    return ImVec4(v.x / 255., v.y / 255., v.z / 255., v.w / 255);
}

static const char *VSHADER =
    "                                                               \n"
    "attribute vec3 a_pos;                                          \n"
    "attribute vec2 a_tex_pos;                                      \n"
    "attribute vec4 a_color;                                        \n"
    "                                                               \n"
    "uniform mat4 u_proj_mat;                                       \n"
    "                                                               \n"
    "varying vec2 v_tex_pos;                                        \n"
    "varying vec4 v_color;                                          \n"
    "                                                               \n"
    "void main()                                                    \n"
    "{                                                              \n"
    "    gl_Position = u_proj_mat * vec4(a_pos, 1.0);               \n"
    "    v_tex_pos = a_tex_pos;                                     \n"
    "    v_color = a_color;                                         \n"
    "}                                                              \n"
;

static const char *FSHADER =
    "                                                               \n"
    "#ifdef GL_ES                                                   \n"
    "precision mediump float;                                       \n"
    "#endif                                                         \n"
    "                                                               \n"
    "uniform sampler2D u_tex;                                       \n"
    "                                                               \n"
    "varying vec2 v_tex_pos;                                        \n"
    "varying vec4 v_color;                                          \n"
    "                                                               \n"
    "void main()                                                    \n"
    "{                                                              \n"
    "    gl_FragColor = v_color * texture2D(u_tex, v_tex_pos);      \n"
    "}                                                              \n"
;

typedef struct {
    GLuint prog;

    GLuint a_pos_l;
    GLuint a_tex_pos_l;
    GLuint a_color_l;

    GLuint u_tex_l;
    GLuint u_proj_mat_l;
} prog_t;

typedef struct gui_t {
    prog_t  prog;
    GLuint  array_buffer;
    GLuint  index_buffer;
    int     min_panel_size;
    struct {
        gesture_t drag;
        gesture_t hover;
    }       gestures;
    bool    mouse_in_view;
    bool    capture_mouse;
} gui_t;

static gui_t *gui = NULL;

// Notify the gui that we want the panel size to be at least as large as
// the last item.
static void auto_adjust_panel_size(void) {
    float w = ImGui::GetItemRectMax().x;
    ImGuiStyle& style = ImGui::GetStyle();
    if (ImGui::GetScrollMaxY() > 0) w += style.ScrollbarSize;
    gui->min_panel_size = max(gui->min_panel_size, w);
}

static void on_click(void) {
    if (DEFINED(GUI_SOUND))
        sound_play("click");
}

static void init_prog(prog_t *p)
{
    p->prog = create_program(VSHADER, FSHADER, NULL);
    GL(glUseProgram(p->prog));
#define UNIFORM(x) p->x##_l = glGetUniformLocation(p->prog, #x);
#define ATTRIB(x) p->x##_l = glGetAttribLocation(p->prog, #x)
    UNIFORM(u_proj_mat);
    UNIFORM(u_tex);
    ATTRIB(a_pos);
    ATTRIB(a_tex_pos);
    ATTRIB(a_color);
#undef ATTRIB
#undef UNIFORM
    GL(glUniform1i(p->u_tex_l, 0));
}


static void render_prepare_context(void)
{
    #define OFFSETOF(TYPE, ELEMENT) ((size_t)&(((TYPE *)0)->ELEMENT))
    // Setup render state: alpha-blending enabled, no face culling, no depth testing, scissor enabled
    GL(glEnable(GL_BLEND));
    GL(glBlendEquation(GL_FUNC_ADD));
    GL(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    GL(glDisable(GL_CULL_FACE));
    GL(glDisable(GL_DEPTH_TEST));
    GL(glEnable(GL_SCISSOR_TEST));
    GL(glActiveTexture(GL_TEXTURE0));

    // Setup orthographic projection matrix
    const float width = ImGui::GetIO().DisplaySize.x;
    const float height = ImGui::GetIO().DisplaySize.y;
    const float ortho_projection[4][4] =
    {
        { 2.0f/width,	0.0f,			0.0f,		0.0f },
        { 0.0f,			2.0f/-height,	0.0f,		0.0f },
        { 0.0f,			0.0f,			-1.0f,		0.0f },
        { -1.0f,		1.0f,			0.0f,		1.0f },
    };
    GL(glUseProgram(gui->prog.prog));
    GL(glUniformMatrix4fv(gui->prog.u_proj_mat_l, 1, 0, &ortho_projection[0][0]));

    GL(glBindBuffer(GL_ARRAY_BUFFER, gui->array_buffer));
    GL(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gui->index_buffer));
    // This could probably be done only at init time.
    GL(glEnableVertexAttribArray(gui->prog.a_pos_l));
    GL(glEnableVertexAttribArray(gui->prog.a_tex_pos_l));
    GL(glEnableVertexAttribArray(gui->prog.a_color_l));
    GL(glVertexAttribPointer(gui->prog.a_pos_l, 2, GL_FLOAT, false,
                             sizeof(ImDrawVert),
                             (void*)OFFSETOF(ImDrawVert, pos)));
    GL(glVertexAttribPointer(gui->prog.a_tex_pos_l, 2, GL_FLOAT, false,
                             sizeof(ImDrawVert),
                             (void*)OFFSETOF(ImDrawVert, uv)));
    GL(glVertexAttribPointer(gui->prog.a_color_l, 4, GL_UNSIGNED_BYTE,
                             true, sizeof(ImDrawVert),
                             (void*)OFFSETOF(ImDrawVert, col)));
    #undef OFFSETOF
}

static void ImImpl_RenderDrawLists(ImDrawData* draw_data)
{
    const float height = ImGui::GetIO().DisplaySize.y;
    render_prepare_context();
    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        const ImDrawIdx* idx_buffer_offset = 0;

        GL(glBufferData(GL_ARRAY_BUFFER,
                    (GLsizeiptr)cmd_list->VtxBuffer.size() * sizeof(ImDrawVert),
                    (GLvoid*)&cmd_list->VtxBuffer.front(), GL_DYNAMIC_DRAW));

        GL(glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                    (GLsizeiptr)cmd_list->IdxBuffer.size() * sizeof(ImDrawIdx),
                    (GLvoid*)&cmd_list->IdxBuffer.front(), GL_DYNAMIC_DRAW));

        for (const ImDrawCmd* pcmd = cmd_list->CmdBuffer.begin(); pcmd != cmd_list->CmdBuffer.end(); pcmd++)
        {
            if (pcmd->UserCallback)
            {
                pcmd->UserCallback(cmd_list, pcmd);
                render_prepare_context(); // Restore context.
            }
            else
            {
                GL(glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)pcmd->TextureId));
                GL(glScissor((int)pcmd->ClipRect.x, (int)(height - pcmd->ClipRect.w),
                             (int)(pcmd->ClipRect.z - pcmd->ClipRect.x),
                             (int)(pcmd->ClipRect.w - pcmd->ClipRect.y)));
                GL(glDrawElements(GL_TRIANGLES, (GLsizei)pcmd->ElemCount,
                                  GL_UNSIGNED_SHORT, idx_buffer_offset));
            }
            idx_buffer_offset += pcmd->ElemCount;
        }
    }
    GL(glDisable(GL_SCISSOR_TEST));
}

static void load_fonts_texture()
{
    ImGuiIO& io = ImGui::GetIO();

    unsigned char* pixels;
    int width, height;
    const void *data;
    int data_size;
    ImFontConfig conf;

    const ImWchar ranges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement
        0x25A0, 0x25FF, // Geometric shapes
        0
    };
    conf.FontDataOwnedByAtlas = false;

    data = assets_get("asset://data/fonts/DejaVuSans-light.ttf", &data_size);
    assert(data);
    io.Fonts->AddFontFromMemoryTTF((void*)data, data_size, 14, &conf, ranges);
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    GLuint tex_id;
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    io.Fonts->TexID = (void *)(intptr_t)tex_id;
}

static void init_ImGui()
{
    float padding_k = 1;
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f/60.0f;

    io.KeyMap[ImGuiKey_Tab]         = KEY_TAB;
    io.KeyMap[ImGuiKey_LeftArrow]   = KEY_LEFT;
    io.KeyMap[ImGuiKey_RightArrow]  = KEY_RIGHT;
    io.KeyMap[ImGuiKey_UpArrow]     = KEY_UP;
    io.KeyMap[ImGuiKey_DownArrow]   = KEY_DOWN;
    io.KeyMap[ImGuiKey_PageUp]      = KEY_PAGE_UP;
    io.KeyMap[ImGuiKey_PageDown]    = KEY_PAGE_DOWN;
    io.KeyMap[ImGuiKey_Home]        = KEY_HOME;
    io.KeyMap[ImGuiKey_End]         = KEY_END;
    io.KeyMap[ImGuiKey_Delete]      = KEY_DELETE;
    io.KeyMap[ImGuiKey_Backspace]   = KEY_BACKSPACE;
    io.KeyMap[ImGuiKey_Enter]       = KEY_ENTER;
    io.KeyMap[ImGuiKey_Escape]      = KEY_ESCAPE;
    io.KeyMap[ImGuiKey_A]           = 'A';
    io.KeyMap[ImGuiKey_C]           = 'C';
    io.KeyMap[ImGuiKey_V]           = 'V';
    io.KeyMap[ImGuiKey_X]           = 'X';
    io.KeyMap[ImGuiKey_Y]           = 'Y';
    io.KeyMap[ImGuiKey_Z]           = 'Z';

    if (DEFINED(__linux__)) {
        io.SetClipboardTextFn = sys_set_clipboard_text;
        io.GetClipboardTextFn = sys_get_clipboard_text;
    }

    io.RenderDrawListsFn = ImImpl_RenderDrawLists;
    load_fonts_texture();

    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding = 4;
    style.WindowRounding = 0;
    style.WindowPadding = ImVec2(4, 4);
    style.ItemSpacing = ImVec2(4, 4);
    style.FramePadding = ImVec2(4 * padding_k, 2 * padding_k);

    style.Colors[ImGuiCol_WindowBg] = IMHEXCOLOR(0x607272FF);
    style.Colors[ImGuiCol_PopupBg] = IMHEXCOLOR(0x626262FF);
    style.Colors[ImGuiCol_Header] = style.Colors[ImGuiCol_WindowBg];
    style.Colors[ImGuiCol_Text] = IMHEXCOLOR(0x000000FF);
    style.Colors[ImGuiCol_Button] = IMHEXCOLOR(0xA1A1A1FF);
    style.Colors[ImGuiCol_FrameBg] = IMHEXCOLOR(0xA1A1A1FF);
    style.Colors[ImGuiCol_ButtonActive] = IMHEXCOLOR(0x6666CCFF);
    style.Colors[ImGuiCol_ButtonHovered] = IMHEXCOLOR(0x7777CCFF);
    style.Colors[ImGuiCol_CheckMark] = IMHEXCOLOR(0x00000AA);
    style.Colors[ImGuiCol_ComboBg] = IMHEXCOLOR(0x727272FF);
    style.Colors[ImGuiCol_MenuBarBg] = IMHEXCOLOR(0x607272FF);
}


static int on_gesture(const gesture_t *gest, void *user)
{
    gui_t *gui = (gui_t*)user;
    ImGuiIO& io = ImGui::GetIO();
    io.MousePos = ImVec2(gest->pos.x, gest->pos.y);
    io.MouseDown[0] = (gest->type == GESTURE_DRAG) &&
                      (gest->state != GESTURE_END);
    if (gest->state == GESTURE_BEGIN && !gui->mouse_in_view)
        gui->capture_mouse = true;
    if (gest->state == GESTURE_END)
        gui->capture_mouse = false;
    return 0;
}


void gui_init(void)
{
    gui = (gui_t*)calloc(1, sizeof(*gui));
    init_prog(&gui->prog);
    GL(glGenBuffers(1, &gui->array_buffer));
    GL(glGenBuffers(1, &gui->index_buffer));
    init_ImGui();
    gui->gestures.drag.type = GESTURE_DRAG;
    gui->gestures.drag.callback = on_gesture;
    gui->gestures.hover.type = GESTURE_HOVER;
    gui->gestures.hover.callback = on_gesture;

    g_tex_icons = texture_new_image("asset://data/icons.png", TF_NEAREST);
    GL(glBindTexture(GL_TEXTURE_2D, g_tex_icons->tex));
}

void gui_release(void)
{
    ImGui::Shutdown();
}

// XXX: Move this somewhere else.
void render_axis_arrows(goxel_t *goxel, const vec2_t *view_size)
{
    const vec3_t AXIS[] = {vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1)};
    int i;
    const int d = 40;  // Distance to corner of the view.
    vec2_t spos = vec2(d, d);
    vec3_t pos, normal, b;
    uvec4b_t c;
    float s = 1;
    vec4_t view = vec4(0, 0, view_size->x, view_size->y);
    camera_get_ray(&goxel->camera, &spos, &view, &pos, &normal);
    if (goxel->camera.ortho)
        s = goxel->camera.dist / 32;
    else
        vec3_iaddk(&pos, normal, 100);

    for (i = 0; i < 3; i++) {
        b = vec3_addk(pos, AXIS[i], 2.0 * s);
        c = uvec4b(AXIS[i].x * 255, AXIS[i].y * 255, AXIS[i].z * 255, 255);
        render_line(&goxel->rend, &pos, &b, &c);
    }
}

typedef struct view {
    goxel_t *goxel;
    vec4_t  rect;
} view_t;

void render_view(const ImDrawList* parent_list, const ImDrawCmd* cmd)
{
    view_t *view = (view_t*)cmd->UserCallbackData;
    const float width = ImGui::GetIO().DisplaySize.x;
    const float height = ImGui::GetIO().DisplaySize.y;
    int rect[4] = {(int)view->rect.x,
                   (int)(height - view->rect.y - view->rect.w),
                   (int)view->rect.z,
                   (int)view->rect.w};
    vec4_t back_color;
    back_color = uvec4b_to_vec4(view->goxel->back_color);

    goxel_render_view(view->goxel, &view->rect);
    render_render(&view->goxel->rend, rect, &back_color);
    GL(glViewport(0, 0, width, height));
}

// XXX: better replace this by something more automatic.
static void auto_grid(int nb, int i, int col)
{
    if ((i + 1) % col != 0) ImGui::SameLine();
}

static void tools_panel(goxel_t *goxel)
{
    const struct {
        int         tool;
        const char  *tool_id;
        const char  *name;
        int         icon;
    } values[] = {
        {TOOL_BRUSH,        "brush",     "Brush",        ICON_TOOL_BRUSH},
        {TOOL_SHAPE,        "shape",     "Shape",        ICON_TOOL_SHAPE},
        {TOOL_LASER,        "laser",     "Laser",        ICON_TOOL_LASER},
        {TOOL_SET_PLANE,    "plane",     "Plane",        ICON_TOOL_PLANE},
        {TOOL_MOVE,         "move",      "Move",         ICON_TOOL_MOVE},
        {TOOL_PICK_COLOR,   "pick_color","Pick Color",   ICON_TOOL_PICK},
        {TOOL_SELECTION,    "selection", "Selection",    ICON_TOOL_SELECTION},
        {TOOL_PROCEDURAL,   "procedural","Procedural",   ICON_TOOL_PROCEDURAL},
    };
    const int nb = ARRAY_SIZE(values);
    int i;
    bool v;
    char action_id[64];
    char label[64];
    const action_t *action = NULL;

    ImGui::GoxGroupBegin();
    for (i = 0; i < nb; i++) {
        v = goxel->tool->id == values[i].tool;
        sprintf(label, "%s", values[i].name);
        if (values[i].tool_id) {
            sprintf(action_id, "tool_set_%s", values[i].tool_id);
            action = action_get(action_id);
            assert(action);
            if (action->shortcut)
                sprintf(label, "%s (%s)", values[i].name, action->shortcut);
        }
        if (gui_selectable_icon(label, &v, values[i].icon)) {
            action_exec(action, "");
        }
        auto_grid(nb, i, 4);
    }
    ImGui::GoxGroupEnd();
    auto_adjust_panel_size();

    if (ImGui::GoxCollapsingHeader("Tool Options", NULL, true, true))
        tool_gui(goxel->tool);
}

static void toggle_layer_only_visible(goxel_t *goxel, layer_t *layer)
{
    layer_t *other;
    bool others_all_invisible = true;
    DL_FOREACH(goxel->image->layers, other) {
        if (other == layer) continue;
        if (other->visible) {
            others_all_invisible = false;
            break;
        }
    }
    DL_FOREACH(goxel->image->layers, other)
        other->visible = others_all_invisible;
    layer->visible = true;
}

static void layers_panel(goxel_t *goxel)
{
    layer_t *layer;
    int i = 0;
    bool current;
    DL_FOREACH(goxel->image->layers, layer) {
        ImGui::PushID(i);
        ImGui::AlignFirstTextHeightToWidgets();
        current = goxel->image->active_layer == layer;
        if (ImGui::Selectable(current ? "●" : " ", &current, 0,
                              ImVec2(12, 12))) {
            if (current) {
                goxel->image->active_layer = layer;
                goxel_update_meshes(goxel, -1);
            }
        }
        ImGui::SameLine();
        if (ImGui::Selectable(layer->visible ? "v##v" : " ##v",
                    &layer->visible, 0, ImVec2(12, 12))) {
            if (ImGui::IsKeyDown(KEY_LEFT_SHIFT))
                toggle_layer_only_visible(goxel, layer);
            goxel_update_meshes(goxel, -1);
        }
        ImGui::SameLine();
        ImGui::InputText("##name", layer->name, sizeof(layer->name));
        i++;
        ImGui::PopID();
        auto_adjust_panel_size();
    }
    gui_action_button("img_new_layer", NULL, 0, "");
    ImGui::SameLine();
    gui_action_button("img_del_layer", NULL, 0, "");
    ImGui::SameLine();
    gui_action_button("img_move_layer_up", NULL, 0, "");
    ImGui::SameLine();
    gui_action_button("img_move_layer_down", NULL, 0, "");

    ImGui::GoxGroupBegin();
    gui_action_button("img_duplicate_layer", "Duplicate", 1, "");
    gui_action_button("img_merge_visible_layers", "Merge visible", 1, "");
    ImGui::GoxGroupEnd();
}

static void palette_panel(goxel_t *goxel)
{
    palette_t *p;
    int i, current, nb = 0;
    const char **names;

    DL_COUNT(goxel->palettes, p, nb);
    names = (const char**)calloc(nb, sizeof(*names));

    i = 0;
    DL_FOREACH(goxel->palettes, p) {
        if (p == goxel->palette) current = i;
        names[i++] = p->name;
    }
    ImGui::PushItemWidth(-1);
    if (ImGui::Combo("", &current, names, nb)) {
        goxel->palette = goxel->palettes;
        for (i = 0; i < current; i++) goxel->palette = goxel->palette->next;
    }
    ImGui::PopItemWidth();
    free(names);

    p = goxel->palette;

    for (i = 0; i < p->size; i++) {
        ImGui::PushID(i);
        ImGui::GoxPaletteEntry(&p->entries[i].color, &goxel->painter.color);
        auto_adjust_panel_size();
        if ((i + 1) % 6 && i != p->size - 1) ImGui::SameLine();
        ImGui::PopID();
    }
}

static void render_advanced_panel(goxel_t *goxel)
{
    float v;
    ImVec4 c;
    int i;
    const struct {
        uvec4b_t   *color;
        const char *label;
    } COLORS[] = {
        {&goxel->back_color, "Back color"},
        {&goxel->grid_color, "Grid color"},
        {&goxel->image_box_color, "Box color"},
    };

    ImGui::Text("Light");
    ImGui::GoxInputAngle("Pitch", &goxel->rend.light.pitch, -90, +90);
    ImGui::GoxInputAngle("Yaw", &goxel->rend.light.yaw, 0, 360);
    gui_checkbox("Fixed", &goxel->rend.light.fixed, NULL);

    ImGui::GoxGroupBegin();
    v = goxel->rend.settings.border_shadow;
    if (gui_input_float("bshadow", &v, 0.1, 0.0, 1.0, NULL)) {
        v = clamp(v, 0, 1); \
        goxel->rend.settings.border_shadow = v;
    }
#define MAT_FLOAT(name, min, max) \
    v = goxel->rend.settings.name;  \
    if (gui_input_float(#name, &v, 0.1, min, max, NULL)) { \
        v = clamp(v, min, max); \
        goxel->rend.settings.name = v; \
    }

    MAT_FLOAT(ambient, 0, 1);
    MAT_FLOAT(diffuse, 0, 1);
    MAT_FLOAT(specular, 0, 1);
    MAT_FLOAT(shininess, 0.1, 10);
    MAT_FLOAT(smoothness, 0, 1);

#undef MAT_FLOAT
    ImGui::GoxGroupEnd();

    ImGui::CheckboxFlags("Borders",
            (unsigned int*)&goxel->rend.settings.effects, EFFECT_BORDERS);
    ImGui::CheckboxFlags("Borders all",
            (unsigned int*)&goxel->rend.settings.effects, EFFECT_BORDERS_ALL);
    ImGui::CheckboxFlags("See back",
            (unsigned int*)&goxel->rend.settings.effects, EFFECT_SEE_BACK);
    if (ImGui::CheckboxFlags("Marching Cubes",
            (unsigned int*)&goxel->rend.settings.effects, EFFECT_MARCHING_CUBES)) {
        goxel->rend.settings.smoothness = 1;
    }
    if (goxel->rend.settings.effects & EFFECT_MARCHING_CUBES)
        ImGui::CheckboxFlags("Flat",
            (unsigned int*)&goxel->rend.settings.effects, EFFECT_FLAT);

    ImGui::Text("Other");
    for (i = 0; i < (int)ARRAY_SIZE(COLORS); i++) {
        ImGui::PushID(COLORS[i].label);
        c = uvec4b_to_imvec4(*COLORS[i].color);
        ImGui::ColorButton(c);
        if (ImGui::BeginPopupContextItem("color context menu", 0)) {
            ImGui::GoxColorEdit("##edit", COLORS[i].color);
            if (ImGui::Button("Close"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::Text("%s", COLORS[i].label);
        ImGui::PopID();
    }
    ImGui::GoxCheckbox("grid_visible", "Show grid");
}


static void render_panel(goxel_t *goxel)
{
    int i, current = 0;
    int nb = render_get_default_settings(0, NULL, NULL);
    float v;
    char *name;
    render_settings_t settings;

    ImGui::Checkbox("Ortho", &goxel->camera.ortho);
    for (i = 0; i < nb; i++) {
        render_get_default_settings(i, &name, &settings);
        current = memcmp(&goxel->rend.settings, &settings,
                         sizeof(settings)) == 0;
        if (ImGui::RadioButton(name, current) && !current)
            goxel->rend.settings = settings;
    }
    v = goxel->rend.settings.shadow;
    if (ImGui::GoxInputFloat("shadow", &v, 0.1)) {
        goxel->rend.settings.shadow = clamp(v, 0, 1);
    }
    if (ImGui::GoxCollapsingHeader("Render Advanced", NULL, true, false))
        render_advanced_panel(goxel);
}

static void export_panel(goxel_t *goxel)
{
    int i;
    int maxsize;
    GL(glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxsize));
    maxsize /= 2; // Because png export already double it.
    goxel->show_export_viewport = true;
    ImGui::GoxGroupBegin();
    i = goxel->image->export_width;
    if (ImGui::GoxInputInt("width", &i, 1, 1, maxsize))
        goxel->image->export_width = clamp(i, 1, maxsize);
    i = goxel->image->export_height;
    if (ImGui::GoxInputInt("height", &i, 1, 1, maxsize))
        goxel->image->export_height = clamp(i, 1, maxsize);
    ImGui::GoxGroupEnd();
}

static void image_panel(goxel_t *goxel)
{
    bool bounded;
    int x, y, z, w, h, d;
    image_t *image = goxel->image;
    box_t *box = &image->box;

    bounded = !box_is_null(*box);
    if (ImGui::Checkbox("Bounded", &bounded)) {
        *box = bounded ? bbox_from_extents(vec3_zero, 16, 16, 16) : box_null;
    }
    if (bounded) {
        w = box->w.x * 2;
        h = box->h.y * 2;
        d = box->d.z * 2;
        x = round(box->p.x - box->w.x);
        y = round(box->p.y - box->h.y);
        z = round(box->p.z - box->d.z);

        gui_group_begin("Origin");
        gui_input_int("x", &x, 0, 0);
        gui_input_int("y", &y, 0, 0);
        gui_input_int("z", &z, 0, 0);
        gui_group_end();

        gui_group_begin("Size");
        gui_input_int("w", &w, 1, 2048);
        gui_input_int("h", &h, 1, 2048);
        gui_input_int("d", &d, 1, 2048);
        gui_group_end();

        *box = bbox_from_extents(
                vec3(x + w / 2., y + h / 2., z + d / 2.),
                w / 2., h / 2., d / 2.);
    }
}

static void cameras_panel(goxel_t *goxel)
{
    camera_t *cam;
    int i = 0;
    bool current;
    DL_FOREACH(goxel->image->cameras, cam) {
        ImGui::PushID(i);
        ImGui::AlignFirstTextHeightToWidgets();
        current = goxel->image->active_camera == cam;
        if (ImGui::Selectable(current ? "●" : " ", &current, 0,
                              ImVec2(12, 12))) {
            if (current) {
                camera_set(&goxel->camera, cam);
                goxel->image->active_camera = cam;
            } else {
                goxel->image->active_camera = NULL;
            }
        }
        ImGui::SameLine();
        ImGui::InputText("##name", cam->name, sizeof(cam->name));
        ImGui::PopID();
        i++;
    }
    gui_action_button("img_new_camera", NULL, 0, "");
    ImGui::SameLine();
    gui_action_button("img_del_camera", NULL, 0, "");
    ImGui::SameLine();
    gui_action_button("img_move_camera_up", NULL, 0, "");
    ImGui::SameLine();
    gui_action_button("img_move_camera_down", NULL, 0, "");

    cam = &goxel->camera;
    ImGui::GoxInputFloat("dist", &cam->dist, 10.0);

    ImGui::GoxGroupBegin("Offset");
    ImGui::GoxInputFloat("x", &cam->ofs.x, 1.0);
    ImGui::GoxInputFloat("y", &cam->ofs.y, 1.0);
    ImGui::GoxInputFloat("z", &cam->ofs.z, 1.0);
    ImGui::GoxGroupEnd();

    ImGui::GoxInputQuat("Rotation", &cam->rot);

    ImGui::GoxGroupBegin("Set");
    ImGui::GoxAction("view_left", "left", 0.5, ""); ImGui::SameLine();
    ImGui::GoxAction("view_right", "right", 1.0, "");
    ImGui::GoxAction("view_front", "front", 0.5, ""); ImGui::SameLine();
    ImGui::GoxAction("view_top", "top", 1.0, "");
    ImGui::GoxAction("view_default", "default", 1.0, "");
    ImGui::GoxGroupEnd();
}

static void import_image_plane(goxel_t *goxel)
{
    const char *path;
    path = noc_file_dialog_open(NOC_FILE_DIALOG_OPEN,
            "png\0*.png\0jpg\0*.jpg;*.jpeg\0", NULL, NULL);
    if (!path) return;
    goxel_import_image_plane(goxel, path);
}

static void shift_alpha_popup(goxel_t *goxel)
{
    static int v = 0;
    static mesh_t *original_mesh = NULL;
    mesh_t *mesh;
    mesh = goxel->image->active_layer->mesh;
    if (!original_mesh)
        original_mesh = mesh_copy(mesh);
    if (ImGui::InputInt("shift", &v, 1)) {
        mesh_set(mesh, original_mesh);
        mesh_shift_alpha(mesh, v);
        goxel_update_meshes(goxel, -1);
    }
    if (ImGui::Button("OK")) {
        mesh_delete(original_mesh);
        original_mesh = NULL;
        ImGui::CloseCurrentPopup();
    }
}

static void about_popup(void)
{
    ImGui::Text("Goxel " GOXEL_VERSION_STR);
    ImGui::Text("Copyright © 2015-2017");
    ImGui::Text("Guillaume Chereau <guillaume@noctua-software.com>");
    ImGui::Text("GPL 3 License");

    if (ImGui::CollapsingHeader("Credits")) {
        ImGui::Text("Code:");
        ImGui::BulletText("Guillaume Chereau <guillaume@noctua-software.com>");
        ImGui::BulletText("Dustin Willis Webber <dustin.webber@gmail.com>");
        ImGui::BulletText("Pablo Hugo Reda <pabloreda@gmail.com>");
        ImGui::BulletText("Othelarian (https://github.com/othelarian)");
        ImGui::Text("Art:");
        ImGui::BulletText("Michal (https://github.com/YarlBoro)");
    }

    if (ImGui::Button("OK")) {
        ImGui::CloseCurrentPopup();
    }
}

static int check_action_shortcut(const action_t *action, void *user)
{
    ImGuiIO& io = ImGui::GetIO();
    const char *s = action->shortcut;
    bool check_key = true;
    bool check_char = true;
    if (!s) return 0;
    if (io.KeyCtrl) {
        if (!str_startswith(s, "Ctrl")) return 0;
        s += strlen("Ctrl ");
        check_char = false;
    }
    if (io.KeyShift) {
        check_key = false;
    }
    if (    (check_char && ImGui::GoxIsCharPressed(s[0])) ||
            (check_key && ImGui::IsKeyPressed(s[0], false))) {
        action_exec(action, "");
        return 1;
    }
    return 0;
}

static int import_menu_action_callback(const action_t *a, void *user)
{
    if (!a->file_format.name) return 0;
    if (!str_startswith(a->id, "import_")) return 0;
    if (ImGui::MenuItem(a->file_format.name)) action_exec(a, "");
    return 0;
}

static int export_menu_action_callback(const action_t *a, void *user)
{
    if (!a->file_format.name) return 0;
    if (!str_startswith(a->id, "export_")) return 0;
    if (ImGui::MenuItem(a->file_format.name)) action_exec(a, "");
    return 0;
}

static void render_menu(void)
{
    bool popup_about = false;
    bool popup_shift_alpha = false;

    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        ImGui::GoxMenuItem("save", "Save");
        ImGui::GoxMenuItem("save_as", "Save as");
        ImGui::GoxMenuItem("open", "Open");
        if (ImGui::BeginMenu("Import...")) {
            if (ImGui::MenuItem("image plane")) import_image_plane(goxel);
            actions_iter(import_menu_action_callback, NULL);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Export As..")) {
            actions_iter(export_menu_action_callback, NULL);
            ImGui::EndMenu();
        }
        ImGui::GoxMenuItem("quit", "Quit");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Clear", "Delete"))
            action_exec2("layer_clear", "");
        ImGui::GoxMenuItem("undo", "Undo");
        ImGui::MenuItem("redo", "Redo");
        ImGui::GoxMenuItem("copy", "Copy");
        ImGui::GoxMenuItem("past", "Past");
        if (ImGui::MenuItem("Shift Alpha"))
            popup_shift_alpha = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::GoxMenuItem("view_left", "Left");
        ImGui::GoxMenuItem("view_right", "Right");
        ImGui::GoxMenuItem("view_front", "Front");
        ImGui::GoxMenuItem("view_top", "Top");
        ImGui::GoxMenuItem("view_default", "Default");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About"))
            popup_about = true;
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();

    if (popup_about) ImGui::OpenPopup("About");
    if (popup_shift_alpha) ImGui::OpenPopup("Shift Alpha");
}

void gui_iter(goxel_t *goxel, const inputs_t *inputs)
{
    static view_t view;
    static int current_panel = 0;
    float left_pane_width;
    unsigned int i;
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw_list;
    ImGuiStyle& style = ImGui::GetStyle();
    gesture_t *gestures[] = {&gui->gestures.drag, &gui->gestures.hover};
    vec4_t display_rect = vec4(0, 0,
                               goxel->screen_size.x, goxel->screen_size.y);
    io.DisplaySize = ImVec2((float)goxel->screen_size.x,
                            (float)goxel->screen_size.y);
    io.DeltaTime = 1.0 / 60;
    gesture_update(2, gestures, inputs, &display_rect, gui);
    io.MouseWheel = inputs->mouse_wheel;

    for (i = 0; i < ARRAY_SIZE(inputs->keys); i++)
        io.KeysDown[i] = inputs->keys[i];
    io.KeyShift = inputs->keys[KEY_LEFT_SHIFT] ||
                  inputs->keys[KEY_RIGHT_SHIFT];
    io.KeyCtrl = inputs->keys[KEY_CONTROL];
    for (i = 0; i < ARRAY_SIZE(inputs->chars); i++) {
        if (!inputs->chars[i]) break;
        io.AddInputCharacter(inputs->chars[i]);
    }

    ImGui::NewFrame();

    // Create the root fullscreen window.
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar |
                                    ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoScrollbar |
                                    ImGuiWindowFlags_MenuBar |
                                    ImGuiWindowFlags_NoCollapse;

    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y));
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::Begin("Goxel", NULL, window_flags);

    render_menu();

    left_pane_width = max(168, gui->min_panel_size);
    gui->min_panel_size = 0;

    /*
    if (current_panel == 0 && goxel->tool->id == TOOL_PROCEDURAL) {
        left_pane_width = 400;
    }
    */
    ImGui::BeginChild("left pane", ImVec2(left_pane_width, 0), true);

    const struct {
        const char *name;
        void (*fn)(goxel_t *goxel);
    } PANELS[] = {
        {"Tools", tools_panel},
        {"Palette", palette_panel},
        {"Layers", layers_panel},
        {"Render", render_panel},
        {"Cameras", cameras_panel},
        {"Image", image_panel},
        {"Export", export_panel},
    };

    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IMHEXCOLOR(0x607272FF));
    ImGui::PushStyleColor(ImGuiCol_Button, IMHEXCOLOR(0x455656FF));

    draw_list = ImGui::GetWindowDrawList();
    ImVec2 rmin = ImGui::GetCursorScreenPos() - style.FramePadding;
    ImVec2 rmax = rmin + ImVec2(26, ImGui::GetWindowHeight());
    draw_list->AddRectFilled(rmin, rmax,
                ImGui::ColorConvertFloat4ToU32(IMHEXCOLOR(0x304242FF)));

    for (i = 0; i < (int)ARRAY_SIZE(PANELS); i++) {
        bool b = (current_panel == (int)i);
        if (ImGui::GoxTab(PANELS[i].name, &b)) {
            on_click();
            current_panel = i;
        }
    }
    ImGui::PopStyleColor(2);
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::BeginGroup();

    goxel->show_export_viewport = false;

    ImGui::PushID("panel");
    ImGui::PushID(PANELS[current_panel].name);
    PANELS[current_panel].fn(goxel);
    ImGui::PopID();
    ImGui::PopID();

    ImGui::EndGroup();
    ImGui::EndChild();

    if (ImGui::BeginPopupModal("Shift Alpha", NULL,
                ImGuiWindowFlags_AlwaysAutoResize)) {
        shift_alpha_popup(goxel);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("About", NULL,
                ImGuiWindowFlags_AlwaysAutoResize)) {
        about_popup();
        ImGui::EndPopup();
    }

    ImGui::SameLine();

    ImGui::BeginChild("3d view", ImVec2(0, 0), false,
                      ImGuiWindowFlags_NoScrollWithMouse);
    // ImGui::Text("3d view");
    view.goxel = goxel;
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    canvas_size.y -= 20; // Leave space for the help label.
    view.rect = vec4(canvas_pos.x, canvas_pos.y, canvas_size.x, canvas_size.y);
    draw_list = ImGui::GetWindowDrawList();
    draw_list->AddCallback(render_view, &view);
    // Invisible button so that we catch inputs.
    ImGui::InvisibleButton("canvas", canvas_size);
    gui->mouse_in_view = ImGui::IsItemHovered();
    vec2_t view_size = vec2(view.rect.z, view.rect.w);
    vec4_t view_rect = vec4(canvas_pos.x,
                            io.DisplaySize.y - (canvas_pos.y + canvas_size.y),
                            canvas_size.x, canvas_size.y);

    // Call mouse_in_view with inputs in the view referential.
    if (!gui->capture_mouse) {
        inputs_t inputs2 = *inputs;
        for (i = 0; i < ARRAY_SIZE(inputs->touches); i++) {
            inputs2.touches[i].pos.y =
                io.DisplaySize.y - inputs2.touches[i].pos.y;
        }
        goxel_mouse_in_view(goxel, &view_rect, &inputs2);
    }

    render_axis_arrows(goxel, &view_size);

    // Apparently there is a bug if we do not render anything.  So I render
    // a '.' if there is nothing.  This is a hack.
    ImGui::Text("%s", goxel->hint_text ?: ".");
    ImGui::SameLine(180);
    ImGui::Text("%s", goxel->help_text ?: "");

    ImGui::EndChild();

    if (DEBUG) {
        ImGui::SetCursorPos(ImVec2(left_pane_width + 20, 30));
        ImGui::BeginChild("debug", ImVec2(0, 0), false,
                          ImGuiWindowFlags_NoInputs);
        ImGui::Text("Blocks: %d (%.2g MiB)", goxel->block_count,
                (float)goxel->block_count * sizeof(block_data_t) / MiB);
        ImGui::Text("uid: %lu", (unsigned long)goxel->next_uid);
        ImGui::EndChild();
    }

    ImGui::End();

    // Handle the shortcuts.  XXX: this should be done with actions.
    if (ImGui::IsKeyPressed(KEY_DELETE, false))
        action_exec2("layer_clear", "");
    if (ImGui::IsKeyPressed(' ', false) && goxel->painter.mode == MODE_OVER)
        goxel->painter.mode = MODE_SUB;
    if (ImGui::IsKeyReleased(' ') && goxel->painter.mode == MODE_SUB)
        goxel->painter.mode = MODE_OVER;
    if (ImGui::IsKeyReleased(' ') && goxel->painter.mode == MODE_SUB)
        goxel->painter.mode = MODE_OVER;
    float last_tool_radius = goxel->tool_radius;

    if (!io.WantCaptureKeyboard) {
        if (ImGui::GoxIsCharPressed('[')) goxel->tool_radius -= 0.5;
        if (ImGui::GoxIsCharPressed(']')) goxel->tool_radius += 0.5;
        if (goxel->tool_radius != last_tool_radius) {
            goxel->tool_radius = clamp(goxel->tool_radius, 0.5, 64);
        }

        // XXX: this won't map correctly to a French keyboard.  Unfortunately as
        // far as I can tell, GLFW3 does not allow to check for ctrl-Z on any
        // layout on Windows.  For the moment I just ignore the problem until I
        // either find a solution, either find a replacement for GLFW.
        // With the GLUT backend ctrl-z and ctrl-y are actually reported as the
        // key 25 and 26, which might makes more sense.  Here I test for both.
        if (ImGui::GoxIsCharPressed(26)) action_exec2("undo", "");
        if (ImGui::GoxIsCharPressed(25)) action_exec2("redo", "");
        // Check the action shortcuts.
        actions_iter(check_action_shortcut, NULL);
    }
}

void gui_render(void)
{
    ImGui::Render();
}

extern "C" {
using namespace ImGui;

void gui_group_begin(const char *label)
{
    GoxGroupBegin(label);
}

void gui_group_end(void)
{
    GoxGroupEnd();
}

bool gui_input_int(const char *label, int *v, int minv, int maxv)
{
    float minvf = minv;
    float maxvf = maxv;
    bool ret;
    if (minv == 0 && maxv == 0) {
        minvf = -FLT_MAX;
        maxvf = +FLT_MAX;
    }
    ret = GoxInputInt(label, v, 1, minvf, maxvf);
    if (ret) on_click();
    return ret;
}

bool gui_input_float(const char *label, float *v, float step,
                     float minv, float maxv, const char *format)
{
    bool ret;
    if (minv == 0.f && maxv == 0.f) {
        minv = -FLT_MAX;
        maxv = +FLT_MAX;
    }
    if (step == 0.f) step = 0.1f;
    if (!format) format = "%.1f";
    ret = GoxInputFloat(label, v, step, minv, maxv, format);
    if (ret) on_click();
    return ret;
}

bool gui_action_button(const char *id, const char *label, float size,
                       const char *sig, ...)
{
    bool ret;
    const action_t *action;
    va_list ap;

    action = action_get(id);
    assert(action);
    PushID(action->id);
    ret = gui_button(label, size, action->icon);
    if (IsItemHovered()) goxel_set_help_text(goxel, action_get(id)->help);
    if (ret) {
        va_start(ap, sig);
        action_execv(action_get(id), sig, ap);
        va_end(ap);
    }
    PopID();
    return ret;
}

bool gui_selectable(const char *name, bool *v, const char *tooltip, float w)
{
    bool ret;
    ImGuiStyle& style = ImGui::GetStyle();
    float h = 14 + 2 * style.FramePadding.y;
    ret = GoxSelectable(name, v, 0, 0, tooltip, ImVec2(w, h));
    if (ret) on_click();
    return ret;
}

bool gui_selectable_icon(const char *name, bool *v, int icon)
{
    bool ret;
    ret = GoxSelectable(name, v, g_tex_icons->tex, icon);
    if (ret) on_click();
    return ret;
}

float gui_get_avail_width(void)
{
    return GetContentRegionAvailWidth();
}

void gui_text(const char *label)
{
    Text("%s", label);
}

void gui_same_line(void)
{
    SameLine();
}

bool gui_color(uvec4b_t *color)
{
    ImVec4 icolor;
    icolor = uvec4b_to_imvec4(*color);
    ImGui::ColorButton(icolor);
    if (ImGui::BeginPopupContextItem("color context menu", 0)) {
        ImGui::GoxColorEdit("##edit", color);
        if (ImGui::Button("Close"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    return false;
}

bool gui_checkbox(const char *label, bool *v, const char *hint)
{
    bool ret;
    ret = Checkbox(label, v);
    if (hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", hint);
    if (ret) on_click();
    return ret;
}

bool gui_button(const char *label, float size, int icon)
{
    bool ret;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 uv0, uv1;
    float h;
    ImVec2 button_size;

    button_size = ImVec2(size * GetContentRegionAvailWidth(), 20);
    if (size == -1) button_size.x = GetContentRegionAvailWidth();
    if (size == 0 && label == NULL) button_size.x = 20;
    label = label ?: "";
    ret = Button(label, button_size);
    if (icon) {
        uv0 = ImVec2((icon % 8) / 8.0, (icon / 8) / 8.0);
        uv1 = ImVec2(uv0.x + 1. / 8, uv0.y + 1. / 8);
        uv0 += ImVec2(3 / 128., 3 / 128.);
        uv1 -= ImVec2(3 / 128., 3 / 128.);
        h = GetItemRectSize().y;
        draw_list->AddImage((void*)(intptr_t)g_tex_icons->tex,
                            GetItemRectMin(),
                            GetItemRectMin() + ImVec2(h, h),
                            uv0, uv1, 0xFF000000);
    }
    if (ret) on_click();
    return ret;
}

bool gui_input_text(const char *label, char *txt, int size)
{
    return InputText(label, txt, size);
}

bool gui_input_text_multiline(const char *label, char *buf, int size,
                              float width, float height)
{
    // We set the frame color to a semi transparent value, because otherwise
    // we cannot render the error highlight.
    // XXX: fix that.
    bool ret;
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4 col = style.Colors[ImGuiCol_FrameBg];
    style.Colors[ImGuiCol_FrameBg].w = 0.5;
    ret = InputTextMultiline(label, buf, size, ImVec2(width, height));
    style.Colors[ImGuiCol_FrameBg] = col;
    auto_adjust_panel_size();
    return ret;
}

bool gui_combo(const char *label, int *v, const char **names, int nb)
{
    return Combo(label, v, names, nb);
}

void gui_input_text_multiline_highlight(int line)
{
    float h = ImGui::CalcTextSize("").y;
    ImVec2 rmin = ImGui::GetItemRectMin();
    ImVec2 rmax = ImGui::GetItemRectMax();
    rmin.y = rmin.y + line * h + 2;
    rmax.y = rmin.y + h;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(rmin, rmax, 0xff0000ff);
}

void gui_enabled_begin(bool enabled)
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4 color = style.Colors[ImGuiCol_Text];
    if (!enabled) color.w /= 2;
    ImGui::PushStyleColor(ImGuiCol_Text, color);
}

void gui_enabled_end(void)
{
    ImGui::PopStyleColor();
}

}
