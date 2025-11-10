#include "convert.hpp"

#include "lodepng/lodepng.h"

#include "model.hpp"

std::vector<std::uint8_t> read_binary_file(char const* filename)
{
    FILE* fp = std::fopen(filename, "rb");
    if(!fp)
        return {};
    auto scope_guard = make_scope_guard([&]{ std::fclose(fp); });

    // Get the file size
    std::fseek(fp, 0, SEEK_END);
    std::size_t const file_size = ftell(fp);
    std::fseek(fp, 0, SEEK_SET);

    std::vector<std::uint8_t> data(file_size);

    if(data.empty() || std::fread(data.data(), file_size, 1, fp) != 1)
        return {};

    return data;
}

std::vector<attr_bitmaps_t> chr_to_bitmaps(std::uint8_t const* data, std::size_t size, std::uint8_t const* palette, 
                                           std::vector<std::uint16_t> const& indices)
{
    std::vector<attr_bitmaps_t> ret;

    //size = std::min<std::size_t>(size, 16*256);

    wxImage bad_image(bad_image_xpm);

    for(unsigned i = 0; i < size; i += 16)
    {
        unsigned j = i / 16;
        if(j >= indices.size() || (j != 0 && indices[j] == indices[j-1]))
        {
            ret.push_back({{
                bad_image,
                bad_image,
                bad_image,
                bad_image,
            }});
            continue;
        }

        std::array<std::array<rgb_t, 8*8>, 4> rgb;

        std::uint8_t const* plane0 = data + i;
        std::uint8_t const* plane1 = data + i + 8;

        for(unsigned y = 0; y < 8; ++y)
        for(unsigned x = 0; x < 8; ++x)
        {
            unsigned const rx = 7 - x;
            unsigned const entry = ((plane0[y] >> rx) & 1) | (((plane1[y] >> rx) << 1) & 0b10);
            assert(entry < 4);

            for(unsigned j = 0; j < 4; ++j)
            {
                std::uint8_t const color = palette[entry + (j*4)] % 64;
                rgb[j][y*8+x] = nes_colors[color];
            }
        }

        ret.push_back({{
            wxImage(8, 8, reinterpret_cast<unsigned char*>(rgb[0].data()), true),
            wxImage(8, 8, reinterpret_cast<unsigned char*>(rgb[1].data()), true),
            wxImage(8, 8, reinterpret_cast<unsigned char*>(rgb[2].data()), true),
            wxImage(8, 8, reinterpret_cast<unsigned char*>(rgb[3].data()), true),
        }});
    }

    return ret;
}

std::pair<std::vector<bitmap_t>, std::vector<wxBitmap>> load_collision_file(wxString const& string, unsigned scale)
{
    if(string.IsEmpty() || scale == 0)
        return {};

    std::pair<std::vector<bitmap_t>, std::vector<wxBitmap>> ret;

    wxLogNull go_away;
    wxImage base(string);
    if(!base.IsOk())
        return {};

    for(coord_t c : dimen_range({4, 64}))
    {
        wxImage tile = base.Copy();
        unsigned const s = 8 * scale;
        tile.Resize({ s, s }, { c.x * -s, c.y * -s }, 255, 0, 255);
#ifdef GC_RENDER
        ret.first.emplace_back(get_renderer()->CreateBitmapFromImage(tile));
#else
        ret.first.emplace_back(tile);
#endif
        ret.second.emplace_back(tile);
    }

    return ret;
}

static std::uint8_t map_grey_alpha(std::uint8_t grey, std::uint8_t alpha, std::uint8_t& transparent)
{
    transparent = alpha < 128;
    return grey >> 6;
}

namespace
{
    class palette_map_t
    {
    public:
        palette_map_t(unsigned char* palette, unsigned num_colors)
        {
            for(unsigned i = 0; i < num_colors; ++i) 
            {
                std::uint8_t const a = palette[4 * i + 3];
                if(a >= 128)
                    map.push_back(i);
            }
        }

        std::uint8_t lookup(std::uint8_t palette) const 
        { 
            for(unsigned i = 0; i < map.size(); i += 1)
                if(palette == map[i])
                    return i;
            return 0;
        }

        std::uint8_t is_alpha(std::uint8_t palette) const 
        {
            for(unsigned i = 0; i < map.size(); i += 1)
                if(palette == map[i])
                    return false;
            return true;
        }

    private:
        std::vector<std::uint8_t> map;
    };
}


chr_patterns_t png_to_chr(std::uint8_t const* png, std::size_t size)
{
    unsigned width, height;
    std::vector<std::uint8_t> image; //the raw pixels
    std::vector<std::uint8_t> transparent;
    lodepng::State state;
    unsigned error;

    if((error = lodepng_inspect(&width, &height, &state, png, size)))
        goto fail;

    if(width % 8 != 0)
        throw std::runtime_error("Image width is not a multiple of 8.");
    else if(height % 8 != 0)
        throw std::runtime_error("Image height is not a multiple of 8.");

    switch(state.info_png.color.colortype)
    {
    case LCT_PALETTE:
        {
            state.info_raw.colortype = LCT_PALETTE;
            if((error = lodepng::decode(image, width, height, state, png, size)))
                goto fail;
            LodePNGColorMode& color = state.info_png.color;
            palette_map_t map(color.palette, color.palettesize);
            unsigned const n = width * height;
            transparent.resize(n);
            for(unsigned i = 0; i < n; i += 1)
            {
                transparent[i] = map.is_alpha(image[i]);
                image[i] = map.lookup(image[i]);
            }
        }
        break;

    case LCT_GREY:
    case LCT_RGB:
        state.info_raw.colortype = LCT_GREY;
        if((error = lodepng::decode(image, width, height, state, png, size)))
            goto fail;
        transparent.resize(width * height, 0);
        for(std::uint8_t& c : image)
            c >>= 6;
        break;

    default:
        state.info_raw.colortype = LCT_GREY_ALPHA;
        if((error = lodepng::decode(image, width, height, state, png, size)))
            goto fail;
        assert(image.size() == width * height * 2);
        unsigned const n = width * height;
        transparent.resize(n);
        for(unsigned i = 0; i < n; ++i)
            image[i] = map_grey_alpha(image[i*2], image[i*2 + 1], transparent[i]);
        image.resize(n);
        break;
    }

    // Now convert to CHR
    {
        std::vector<std::uint8_t> result;
        result.reserve(image.size() / 4);

        std::vector<std::uint16_t> indices;
        indices.reserve(image.size() / 64);
        std::uint16_t index = 0;

        for(unsigned ty = 0; ty < height; ty += 8)
        for(unsigned tx = 0; tx < width; tx += 8, indices.push_back(index))
        {
            bool any_transparent = false;
            bool any_opaque = false;

            for(unsigned y = 0; y < 8; ++y)
            for(unsigned x = 0; x < 8; ++x)
            {
                bool t = transparent[tx + x + (ty + y)*width];
                any_transparent |= t;
                any_opaque      |= !t;
            }

            for(unsigned y = 0; y < 8; ++y)
            {
                std::uint8_t v = 0;
                for(unsigned x = 0; x < 8; ++x)
                    v |= (image[tx + x + (ty + y)*width] & 1) << (7-x);
                result.push_back(v);
            }

            for(unsigned y = 0; y < 8; ++y)
            {
                std::uint8_t v = 0;
                for(unsigned x = 0; x < 8; ++x)
                    v |= (image[tx + x + (ty + y)*width] >> 1) << (7-x);
                result.push_back(v);
            }

            if(any_transparent && !any_opaque)
                continue;

            index += 1;
        }

        return { std::move(result), std::move(indices) };
    }
fail:
    throw std::runtime_error(std::string("png decoder error: ") + lodepng_error_text(error));
}

attr_gc_bitmaps_t convert_bitmap(attr_bitmaps_t const& bmp)
{
#if GC_RENDER
    return {
        get_renderer()->CreateBitmap(bmp[0]),
        get_renderer()->CreateBitmap(bmp[1]),
        get_renderer()->CreateBitmap(bmp[2]),
        get_renderer()->CreateBitmap(bmp[3]),
    };
#else
    return bmp;
#endif
}
