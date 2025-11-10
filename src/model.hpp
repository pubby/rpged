#ifndef MODEL_HPP
#define MODEL_HPP

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <variant>
#include <set>
#include <filesystem>
#include <vector>
#include <unordered_map>

#include "2d/geometry.hpp"
#include "2d/grid.hpp"

#include "convert.hpp"
#include "tool.hpp"

using namespace i2d;

class tile_layer_t;
class metatile_layer_t;
class level_model_t;
struct object_t;
class model_t;

using palette_array_t = std::array<std::uint8_t, 16>;
using chr_array_t = std::array<std::uint8_t, 16*256*4>;

constexpr std::uint8_t ACTIVE_COLLISION = 4;
static constexpr std::size_t UNDO_LIMIT = 256;

inline unsigned chr_id(std::uint32_t tile) { return tile >> 16; }
inline std::uint32_t with_chr_id(std::uint32_t tile, unsigned id) { return (tile & 0xFFFF) | (id << 16); }

inline unsigned tile_tile(std::uint32_t tile) { return tile & 0x3FFF; }
inline unsigned tile_attr(std::uint32_t tile) { return (tile >> 14) & 0b11; }

constexpr char const* bad_image_xpm[] = {
    "8 8 4 1",
    " 	c #390000",
    ".	c #003939",
    "+	c #000039",
    "@	c #390039",
    "  ....++",
    "   ..+++",
    ".   +++.",
    ".. @@+..",
    "..+@@ ..",
    ".+++   .",
    "+++..   ",
    "++....  "};

struct object_t
{
    coord_t position;
    std::string name;
    std::string oclass;
    std::unordered_map<std::string, std::string> fields;

    void append_vec(std::vector<std::uint32_t>& vec) const;
    void from_vec(std::uint32_t const*& ptr, std::uint32_t const* end);
    auto operator<=>(object_t const&) const = default;
};

enum undo_type_t { UNDO, REDO };

struct undo_tiles_t
{
    tile_layer_t* layer;
    rect_t rect;
    std::vector<std::uint32_t> tiles;
};

struct undo_palette_num_t
{
    int num;
};

struct undo_level_dimen_t
{
    class chr_layer_t* layer;
    grid_t<std::uint32_t> tiles;
};

struct undo_new_object_t
{
    level_model_t* level;
    std::deque<unsigned> indices;
};

struct undo_delete_object_t
{
    level_model_t* level;
    std::deque<std::pair<unsigned, object_t>> objects;
};

struct undo_edit_object_t
{
    level_model_t* level;
    unsigned index;
    object_t object;
};

struct undo_move_objects_t
{
    level_model_t* level;
    std::vector<unsigned> indices;
    std::vector<coord_t> positions;
};

using undo_t = std::variant
    < std::monostate
    , undo_tiles_t
    , undo_palette_num_t
    , undo_level_dimen_t
    , undo_new_object_t
    , undo_delete_object_t
    , undo_edit_object_t
    , undo_move_objects_t
    >;

// Used to select and deselect specific tiles:
class select_map_t
{
public:
    select_map_t() = default;
    select_map_t(dimen_t dimen) { resize(dimen); }
    dimen_t dimen() const { return m_selection.dimen(); }

    bool has_selection() const { return (bool)m_select_rect; }
    rect_t select_rect() const { return m_select_rect; }
    auto const& selection() const { return m_selection; }
    auto const& operator[](coord_t c) const { return m_selection.at(c); }

    void select_all(bool select = true);
    void select_invert();
    void select(std::uint8_t tile, bool select_ = true);
    void select_transpose(std::uint8_t tile, bool select_ = true);
    void select(coord_t c, bool select = true);
    void select(rect_t r, bool select = true);

    virtual void resize(dimen_t d) ;

    template<typename Fn>
    void for_each_selected(Fn const& fn)
    {
        for(coord_t c : rect_range(select_rect()))
            if(m_selection[c])
                fn(c);
    }

private:
    void recalc_select_rect(rect_t range);

    rect_t m_select_rect = {};
    grid_t<std::uint8_t> m_selection;
};

enum
{
    LAYER_COLOR,
    LAYER_CHR,
    LAYER_COLLISION,
    LAYER_METATILE,
    LAYER_OBJECTS,
};

struct tile_copy_t
{
    unsigned format;
    std::variant<grid_t<std::uint32_t>, std::vector<object_t>> data;

    std::vector<std::uint32_t> to_vec() const
    {
        if(auto* grid = std::get_if<grid_t<std::uint32_t>>(&data))
        {
            assert(format != LAYER_OBJECTS);
            std::vector<std::uint32_t> vec = { format, grid->dimen().w, grid->dimen().h };
            vec.insert(vec.end(), grid->begin(), grid->end());
            return vec;
        }
        else if(auto* objects = std::get_if<std::vector<object_t>>(&data))
        {
            assert(format == LAYER_OBJECTS);
            std::vector<std::uint32_t> vec = { format, objects->size() };
            for(auto const& object : *objects)
                object.append_vec(vec);
            return vec;
        }

        throw std::runtime_error("Unable to convert clip data to vec.");
    }

    static tile_copy_t from_vec(std::vector<std::uint32_t> const& vec)
    {
        tile_copy_t ret = { vec.at(0) };
        if(ret.format == LAYER_OBJECTS)
        {
            unsigned const size = vec.at(1);
            std::vector<object_t> objects;
            std::uint32_t const* ptr = vec.data() + 2;
            for(unsigned i = 0; i < size; ++i)
                objects.emplace_back().from_vec(ptr, &*vec.end());
            ret.data = std::move(objects);
        }
        else
        {
            grid_t<std::uint32_t> tiles;
            tiles.resize({ vec.at(1), vec.at(2) });
            unsigned i = 3;
            for(coord_t c : dimen_range(tiles.dimen()))
                tiles[c] = vec.at(i++);
            ret.data = std::move(tiles);
        }
        return ret;
    }
};

struct object_copy_t
{
    std::vector<object_t> objects;
};

class tile_layer_t
{
public:
    tile_layer_t(dimen_t picker_dimen, dimen_t canvas_dimen)
    : picker_selector(picker_dimen)
    , canvas_selector(canvas_dimen)
    , tiles(canvas_dimen)
    {}

    virtual unsigned format() const = 0;
    virtual dimen_t tile_size(model_t const& m) const { return { 8, 8 }; }
    virtual dimen_t canvas_dimen() const { return tiles.dimen(); }
    virtual void canvas_resize(dimen_t d) { canvas_selector.resize(d); tiles.resize(d); }
    virtual std::uint32_t get(coord_t c) const { return tiles.at(c); }
    virtual void set(coord_t c, std::uint32_t value) { tiles.at(c) = value; }
    virtual void reset(coord_t c) { set(c, 0); }
    virtual std::uint32_t to_tile(coord_t pick) const { return pick.x + pick.y * picker_selector.dimen().w; }
    virtual coord_t to_pick(std::uint32_t tile) const { return { tile % picker_selector.dimen().w, tile / picker_selector.dimen().w }; }

    virtual tile_copy_t copy(undo_t* cut = nullptr);
    virtual void paste(tile_copy_t const& copy, coord_t at);

    virtual undo_t fill();
    virtual undo_t fill_paste(tile_copy_t const& copy);

    virtual void dropper(coord_t at);

    virtual undo_t save(coord_t at) { return save({ at, picker_selector.dimen() }); }
    virtual undo_t save(rect_t rect);

    template<typename Fn>
    void for_each_picked(coord_t pen_c, Fn const& fn)
    {
        rect_t const select_rect = picker_selector.select_rect();
        picker_selector.for_each_selected([&](coord_t c)
        {
            std::uint32_t const tile = to_tile(c);
            coord_t const at = pen_c + c - select_rect.c;
            if(in_bounds(at, canvas_dimen()))
                fn(at, tile);
        });
    }

    select_map_t picker_selector;
    select_map_t canvas_selector;
    grid_t<std::uint32_t> tiles;
};

class tile_model_t
{
public:
    virtual tile_layer_t& layer() = 0;
    tile_layer_t const& clayer() const { return const_cast<tile_model_t*>(this)->layer(); }
};

////////////////////////////////////////////////////////////////////////////////
// chr /////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

struct chr_file_t
{
    unsigned id;
    std::string name;
    std::filesystem::path path;
    chr_array_t chr = {};
    std::vector<std::uint16_t> indices;

    void load();
};

////////////////////////////////////////////////////////////////////////////////
// color palette ///////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

class color_layer_t : public tile_layer_t
{
public:
    explicit color_layer_t(std::uint32_t& num) 
    : tile_layer_t({ 4, 16 }, { 25, 256 }) 
    , num(num)
    { 
        tiles.fill(0x0F); 

        constexpr unsigned example_palette[25] =
        {
            0x11, 0x2B, 0x39,
            0x13, 0x21, 0x3B,
            0x15, 0x23, 0x31,
            0x17, 0x25, 0x33,

            0x02, 0x14, 0x26,
            0x04, 0x16, 0x28,
            0x06, 0x18, 0x2A,
            0x08, 0x1A, 0x2C,

            0x0F 
        };

        for(unsigned i = 0; i < 25; ++i)
            tiles[{i, 0}] = example_palette[i];
    }

    virtual unsigned format() const override { return LAYER_COLOR; }
    virtual dimen_t tile_size(model_t const& m) const override { return { 16, 16 }; }
    virtual dimen_t canvas_dimen() const override { return { tiles.dimen().w, num }; }
    virtual void reset(coord_t c) override { set(c, 0x0F); }
    virtual std::uint32_t to_tile(coord_t pick) const override { return pick.y + pick.x * picker_selector.dimen().h; }
    virtual coord_t to_pick(std::uint32_t tile) const override { return { tile / picker_selector.dimen().h, tile % picker_selector.dimen().h }; }

    std::uint32_t const& num;
};

class palette_model_t : public tile_model_t
{
public:
    virtual tile_layer_t& layer() override { return color_layer; }

    std::uint32_t num = 1;
    color_layer_t color_layer = color_layer_t(this->num);
};

////////////////////////////////////////////////////////////////////////////////
// levels //////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

class collision_layer_t : public tile_layer_t
{
public:
    collision_layer_t() : tile_layer_t({ 4, 64 }, { 16, 16 }) {}

    virtual unsigned format() const override { return LAYER_COLLISION; }
    virtual dimen_t tile_size(model_t const& m) const;
};

class chr_layer_t : public tile_layer_t
{
public:
    explicit chr_layer_t(unsigned& chr_id, std::uint8_t const& active) 
    : tile_layer_t({ 16, 16*4 }, { 16*3, 16*3 })
    , chr_id(chr_id)
    , active(active)
    {}

    virtual unsigned format() const override { return LAYER_CHR; }
    virtual void reset(coord_t c) { tiles.at(c) = 0; }
    virtual std::uint32_t to_tile(coord_t pick) const { return tile_layer_t::to_tile(pick) | ((active & 0b11) << 14) | (chr_id << 16); }
    virtual coord_t to_pick(std::uint32_t tile) const override { tile &= 0x3FFF; return { tile % picker_selector.dimen().w, tile / picker_selector.dimen().w }; }
    virtual void dropper(coord_t at) override;
    undo_t fill_attribute();

    unsigned& chr_id;
    std::uint8_t const& active;

    undo_t save() { return undo_level_dimen_t{ this, tiles }; }
};

struct class_field_t
{
    std::string type = "U";
    std::string name;
};

struct object_class_t
{
    std::string name;
    std::string macro;
    rgb_t color = { 255, 255, 255 };
    std::deque<class_field_t> fields;
};

enum level_layer_t
{
    ATTR0_LAYER = 0,
    ATTR1_LAYER,
    ATTR2_LAYER,
    ATTR3_LAYER,
    COLLISION_LAYER,
    OBJECT_LAYER,
};

class level_model_t : public tile_model_t
{
public:
    level_model_t() 
    : bad_chr(bad_image_xpm)
    { 
        resize({ 24, 24 }, { 24, 24 }); 
    }

    bool collisions() const { return current_layer == COLLISION_LAYER; }
    virtual tile_layer_t& layer() override { if(collisions()) return collision_layer; else return chr_layer; }
    dimen_t dimen() const { return chr_layer.tiles.dimen(); }
    void resize(dimen_t dimen, dimen_t collision_dimen) 
    {
        chr_layer.canvas_resize(dimen);
        collision_layer.canvas_resize(collision_dimen);
        //collision_layer.tiles.resize(dimen);
        //collision_layer.canvas_selector.resize(dimen);
    }

    void clear_chr();
    void refresh_chr(std::deque<chr_file_t> const& chr_deque, palette_array_t const& palette);

    unsigned count_mt(unsigned metatile_size, unsigned select = 0);

    void reindex_objects();

    std::string name = "level";
    std::string macro_name;
    std::string chr_name;
    unsigned chr_id = 0;
    std::uint8_t palette = 0;
    chr_layer_t chr_layer = chr_layer_t(this->chr_id, this->active);
    collision_layer_t collision_layer;
    std::vector<unsigned> chr_ids;
    std::unordered_map<unsigned, std::vector<attr_gc_bitmaps_t>> chr_bitmaps;
    level_layer_t current_layer = ATTR0_LAYER;
    std::uint8_t active = 0;

    std::set<int> object_selector;
    std::deque<object_t> objects;

    wxImage bad_chr;
};

////////////////////////////////////////////////////////////////////////////////
// model ///////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

struct model_t
{
    model_t()
    {
        chr_files.push_back({ 0, "chr" });
        object_classes.push_back(std::make_shared<object_class_t>("object"));
        auto& level = levels.emplace_back(std::make_shared<level_model_t>());
        level->chr_name = "chr";

    }

    bool modified = false;
    bool modified_since_save = false;
    void modify() { modified = modified_since_save = true; }

    bool show_collisions = false;
    bool show_grid = true;

    wxStatusBar* status_bar = nullptr;

    std::filesystem::path project_path;

    tool_t tool = {};
    std::unique_ptr<tile_copy_t> paste; 

    palette_model_t palette;
    std::deque<std::shared_ptr<level_model_t>> levels;

    std::deque<std::shared_ptr<object_class_t>> object_classes;
    object_t object_picker = {};

    std::deque<chr_file_t> chr_files;

    unsigned metatile_size = 0;
    unsigned collision_scale() const { return std::max<unsigned>(metatile_size, 1); }
    dimen_t collision_div(dimen_t d) const { return vec_div(d + dimen_t{ collision_scale() - 1, collision_scale() - 1 }, collision_scale()); }
    std::filesystem::path collision_path;
    std::vector<bitmap_t> collision_bitmaps;
    std::vector<wxBitmap> collision_wx_bitmaps;

    palette_array_t palette_array(unsigned palette_index = 0);

    // Undo operations:
    undo_t undo(undo_t const& undo) { modify(); return std::visit(*this, undo); }
    undo_t operator()(std::monostate const& m) { return m; }
    undo_t operator()(undo_tiles_t const& undo);
    undo_t operator()(undo_palette_num_t const& undo);
    undo_t operator()(undo_level_dimen_t const& undo);
    undo_t operator()(undo_new_object_t const& undo);
    undo_t operator()(undo_delete_object_t const& undo);
    undo_t operator()(undo_edit_object_t const& undo);
    undo_t operator()(undo_move_objects_t const& undo);

    void write_file(FILE* fp, std::filesystem::path base_path) const;
    void read_file(FILE* fp, std::filesystem::path base_path);

    void write_json(FILE* fp, std::filesystem::path base_path) const;
    void read_json(FILE* fp, std::filesystem::path base_path);
};

struct undo_history_t
{
    std::deque<undo_t> history[2];

    template<undo_type_t U>
    void undo(model_t& model) 
    { 
        if(history[U].empty())
            return;
        history[!U].push_front(model.undo(history[U].front()));
        history[U].pop_front();
    }

    void cull(); 
    void push(undo_t const& undo); 
    bool empty(undo_type_t U) const { return history[U].empty(); }

    template<typename T>
    bool on_top() const
    {
        if(history[UNDO].empty())
            return false;
        return std::holds_alternative<T>(history[UNDO].front());
    }
};

template<typename C>
typename C::value_type* lookup_name(std::string const& name, C& c)
{
    for(auto& e : c)
        if(e.name == name)
            return &e;
    return nullptr;
}

template<typename C>
typename C::value_type lookup_name_ptr(std::string const& name, C& c)
{
    for(auto& e : c)
        if(e->name == name)
            return e;
    return typename C::value_type();
}

#endif
