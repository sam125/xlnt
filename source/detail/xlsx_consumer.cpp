// Copyright (c) 2014-2017 Thomas Fussell
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, WRISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE
//
// @license: http://www.opensource.org/licenses/mit-license.php
// @author: see AUTHORS file

#include <cctype>
#include <numeric> // for std::accumulate

#include <xlnt/cell/cell.hpp>
#include <xlnt/packaging/manifest.hpp>
#include <xlnt/utils/path.hpp>
#include <xlnt/workbook/workbook.hpp>
#include <xlnt/worksheet/worksheet.hpp>
#include <detail/constants.hpp>
#include <detail/custom_value_traits.hpp>
#include <detail/vector_streambuf.hpp>
#include <detail/workbook_impl.hpp>
#include <detail/xlsx_consumer.hpp>
#include <detail/zstream.hpp>

namespace std {

/// <summary>
/// Allows xml::qname to be used as a key in a std::unordered_map.
/// </summary>
template <>
struct hash<xml::qname>
{
    std::size_t operator()(const xml::qname &k) const
    {
        static std::hash<std::string> hasher;
        return hasher(k.string());
    }
};

} // namespace std

namespace {

std::array<xlnt::optional<xlnt::rich_text>, 3> parse_header_footer(const std::string &hf_string)
{
    std::array<xlnt::optional<xlnt::rich_text>, 3> result;

    if (hf_string.empty())
    {
        return result;
    }

    enum class hf_code
    {
        left_section, // &L
        center_section, // &C
        right_section, // &R
        current_page_number, // &P
        total_page_number, // &N
        font_size, // &#
        text_font_color, // &KRRGGBB or &KTTSNN
        text_strikethrough, // &S
        text_superscript, // &X
        text_subscript, // &Y
        date, // &D
        time, // &T
        picture_as_background, // &G
        text_single_underline, // &U
        text_double_underline, // &E
        workbook_file_path, // &Z
        workbook_file_name, // &F
        sheet_tab_name, // &A
        add_to_page_number, // &+
        subtract_from_page_number, // &-
        text_font_name, // &"font name,font type"
        bold_font_style, // &B
        italic_font_style, // &I
        outline_style, // &O
        shadow_style, // &H
        text // everything else
    };

    struct hf_token
    {
        hf_code code = hf_code::text;
        std::string value;
    };

    std::vector<hf_token> tokens;
    std::size_t position = 0;

    while (position < hf_string.size())
    {
        hf_token token;

        auto next_ampersand = hf_string.find('&', position + 1);
        token.value = hf_string.substr(position, next_ampersand - position);
        auto next_position = next_ampersand;

        if (hf_string[position] == '&')
        {
            token.value.clear();
            next_position = position + 2;
            auto first_code_char = hf_string[position + 1];

            if (first_code_char == '"')
            {
                auto end_quote_index = hf_string.find('"', position + 2);
                next_position = end_quote_index + 1;

                token.value = hf_string.substr(position + 2, end_quote_index - position - 2); // remove quotes
                token.code = hf_code::text_font_name;
            }
            else if (first_code_char == '&')
            {
                token.value = "&&"; // escaped ampersand
            }
            else if (first_code_char == 'L')
            {
                token.code = hf_code::left_section;
            }
            else if (first_code_char == 'C')
            {
                token.code = hf_code::center_section;
            }
            else if (first_code_char == 'R')
            {
                token.code = hf_code::right_section;
            }
            else if (first_code_char == 'P')
            {
                token.code = hf_code::current_page_number;
            }
            else if (first_code_char == 'N')
            {
                token.code = hf_code::total_page_number;
            }
            else if (std::string("0123456789").find(hf_string[position + 1]) != std::string::npos)
            {
                token.code = hf_code::font_size;
                next_position = hf_string.find_first_not_of("0123456789", position + 1);
                token.value = hf_string.substr(position + 1, next_position - position - 1);
            }
            else if (first_code_char == 'K')
            {
                if (hf_string[position + 4] == '+' || hf_string[position + 4] == '-')
                {
                    token.value = hf_string.substr(position + 2, 5);
                    next_position = position + 7;
                }
                else
                {
                    token.value = hf_string.substr(position + 2, 6);
                    next_position = position + 8;
                }

                token.code = hf_code::text_font_color;
            }
            else if (first_code_char == 'S')
            {
                token.code = hf_code::text_strikethrough;
            }
            else if (first_code_char == 'X')
            {
                token.code = hf_code::text_superscript;
            }
            else if (first_code_char == 'Y')
            {
                token.code = hf_code::text_subscript;
            }
            else if (first_code_char == 'D')
            {
                token.code = hf_code::date;
            }
            else if (first_code_char == 'T')
            {
                token.code = hf_code::time;
            }
            else if (first_code_char == 'G')
            {
                token.code = hf_code::picture_as_background;
            }
            else if (first_code_char == 'U')
            {
                token.code = hf_code::text_single_underline;
            }
            else if (first_code_char == 'E')
            {
                token.code = hf_code::text_double_underline;
            }
            else if (first_code_char == 'Z')
            {
                token.code = hf_code::workbook_file_path;
            }
            else if (first_code_char == 'F')
            {
                token.code = hf_code::workbook_file_name;
            }
            else if (first_code_char == 'A')
            {
                token.code = hf_code::sheet_tab_name;
            }
            else if (first_code_char == '+')
            {
                token.code = hf_code::add_to_page_number;
            }
            else if (first_code_char == '-')
            {
                token.code = hf_code::subtract_from_page_number;
            }
            else if (first_code_char == 'B')
            {
                token.code = hf_code::bold_font_style;
            }
            else if (first_code_char == 'I')
            {
                token.code = hf_code::italic_font_style;
            }
            else if (first_code_char == 'O')
            {
                token.code = hf_code::outline_style;
            }
            else if (first_code_char == 'H')
            {
                token.code = hf_code::shadow_style;
            }
        }

        position = next_position;
        tokens.push_back(token);
    }

    const auto parse_section = [&tokens, &result](hf_code code)
    {
        std::vector<hf_code> end_codes{hf_code::left_section, hf_code::center_section, hf_code::right_section};
        end_codes.erase(std::find(end_codes.begin(), end_codes.end(), code));

        std::size_t start_index = 0;

        while (start_index < tokens.size() && tokens[start_index].code != code)
        {
            ++start_index;
        }

        if (start_index == tokens.size())
        {
            return;
        }

        ++start_index; // skip the section code
        std::size_t end_index = start_index;

        while (end_index < tokens.size()
            && std::find(end_codes.begin(), end_codes.end(), tokens[end_index].code) == end_codes.end())
        {
            ++end_index;
        }

        xlnt::rich_text current_text;
        xlnt::rich_text_run current_run;

        // todo: all this nice parsing and the codes are just being turned back into text representations
        // It would be nice to create an interface for the library to read and write these codes

        for (auto i = start_index; i < end_index; ++i)
        {
            const auto &current_token = tokens[i];

            if (current_token.code == hf_code::text)
            {
                current_run.first = current_run.first + current_token.value;
                continue;
            }

            if (!current_run.first.empty())
            {
                current_text.add_run(current_run);
                current_run = xlnt::rich_text_run();
            }

            switch (current_token.code)
            {
            case hf_code::text:
                {
                    break; // already handled above
                }

            case hf_code::left_section:
                {
                    break; // used below
                }

            case hf_code::center_section:
                {
                    break; // used below
                }

            case hf_code::right_section:
                {
                    break; // used below
                }

            case hf_code::current_page_number:
                {
                    current_run.first = current_run.first + "&P";
                    break;
                }

            case hf_code::total_page_number:
                {
                    current_run.first = current_run.first + "&N";
                    break;
                }

            case hf_code::font_size:
                {
                    if (!current_run.second.is_set())
                    {
                        current_run.second = xlnt::font();
                    }

                    current_run.second.get().size(std::stod(current_token.value));

                    break;
                }

            case hf_code::text_font_color:
                {
                    if (current_token.value.size() == 6)
                    {
                        if (!current_run.second.is_set())
                        {
                            current_run.second = xlnt::font();
                        }

                        current_run.second.get().color(xlnt::rgb_color(current_token.value));
                    }

                    break;
                }

            case hf_code::text_strikethrough:
                {
                    break;
                }

            case hf_code::text_superscript:
                {
                    break;
                }

            case hf_code::text_subscript:
                {
                    break;
                }

            case hf_code::date:
                {
                    current_run.first = current_run.first + "&D";
                    break;
                }

            case hf_code::time:
                {
                    current_run.first = current_run.first + "&T";
                    break;
                }

            case hf_code::picture_as_background:
                {
                    current_run.first = current_run.first + "&G";
                    break;
                }

            case hf_code::text_single_underline:
                {
                    break;
                }

            case hf_code::text_double_underline:
                {
                    break;
                }

            case hf_code::workbook_file_path:
                {
                    current_run.first = current_run.first + "&Z";
                    break;
                }

            case hf_code::workbook_file_name:
                {
                    current_run.first = current_run.first + "&F";
                    break;
                }

            case hf_code::sheet_tab_name:
                {
                    current_run.first = current_run.first + "&A";
                    break;
                }

            case hf_code::add_to_page_number:
                {
                    break;
                }

            case hf_code::subtract_from_page_number:
                {
                    break;
                }

            case hf_code::text_font_name:
                {
                    auto comma_index = current_token.value.find(',');
                    auto font_name = current_token.value.substr(0, comma_index);

                    if (!current_run.second.is_set())
                    {
                        current_run.second = xlnt::font();
                    }

                    if (font_name != "-")
                    {
                        current_run.second.get().name(font_name);
                    }

                    if (comma_index != std::string::npos)
                    {
                        auto font_type = current_token.value.substr(comma_index + 1);

                        if (font_type == "Bold")
                        {
                            current_run.second.get().bold(true);
                        }
                        else if (font_type == "Italic")
                        {
                            // TODO
                        }
                        else if (font_type == "BoldItalic")
                        {
                            current_run.second.get().bold(true);
                        }
                    }

                    break;
                }

            case hf_code::bold_font_style:
                {
                    if (!current_run.second.is_set())
                    {
                        current_run.second = xlnt::font();
                    }

                    current_run.second.get().bold(true);

                    break;
                }

            case hf_code::italic_font_style:
                {
                    break;
                }

            case hf_code::outline_style:
                {
                    break;
                }

            case hf_code::shadow_style:
                {
                    break;
                }
            }
        }

        if (!current_run.first.empty())
        {
            current_text.add_run(current_run);
        }

        auto location_index =
            static_cast<std::size_t>(code == hf_code::left_section ? 0 : code == hf_code::center_section ? 1 : 2);

        if (!current_text.plain_text().empty())
        {
            result[location_index] = current_text;
        }
    };

    parse_section(hf_code::left_section);
    parse_section(hf_code::center_section);
    parse_section(hf_code::right_section);

    return result;
}

xml::qname qn(const std::string &namespace_, const std::string &name)
{
    return xml::qname(xlnt::constants::ns(namespace_), name);
}

} // namespace

namespace {

#ifndef NDEBUG
#define THROW_ON_INVALID_XML
#endif

#ifdef THROW_ON_INVALID_XML
#define unexpected_element(element) throw xlnt::exception(element.string());
#else
#define unexpected_element(element) skip_remaining_content(element);
#endif

/// <summary>
/// Returns true if bool_string represents a true xsd:boolean.
/// </summary>
bool is_true(const std::string &bool_string)
{
    if (bool_string == "1" || bool_string == "true")
    {
        return true;
    }

#ifdef THROW_ON_INVALID_XML
    if (bool_string == "0" || bool_string == "false")
    {
        return false;
    }

    throw xlnt::exception("xsd:boolean should be one of: 0, 1, true, or false, found " + bool_string);
#else

    return false;
#endif
}

/// <summary>
/// Helper template function that returns true if element is in container.
/// </summary>
template <typename T>
bool contains(const std::vector<T> &container, const T &element)
{
    return std::find(container.begin(), container.end(), element) != container.end();
}

} // namespace

/*
class parsing_context
{
public:
    parsing_context(xlnt::detail::zip_file_reader &archive, const std::string &filename)
        : parser_(stream_, filename)
    {
    }

    xml::parser &parser();

private:
    std::istream stream_;
    xml::parser parser_;
};
*/

namespace xlnt {
namespace detail {

xlsx_consumer::xlsx_consumer(workbook &target)
    : target_(target),
      parser_(nullptr)
{
}

void xlsx_consumer::read(std::istream &source)
{
    archive_.reset(new izstream(source));
    populate_workbook();
}

xml::parser &xlsx_consumer::parser()
{
    return *parser_;
}

std::vector<relationship> xlsx_consumer::read_relationships(const path &part)
{
    const auto part_rels_path = part.parent().append("_rels")
        .append(part.filename() + ".rels").relative_to(path("/"));

    std::vector<xlnt::relationship> relationships;
    if (!archive_->has_file(part_rels_path)) return relationships;

    auto rels_streambuf = archive_->open(part_rels_path);
    std::istream rels_stream(rels_streambuf.get());
    xml::parser parser(rels_stream, part_rels_path.string());
    parser_ = &parser;

    expect_start_element(qn("relationships", "Relationships"), xml::content::complex);

    while (in_element(qn("relationships", "Relationships")))
    {
        expect_start_element(qn("relationships", "Relationship"), xml::content::simple);

        const auto target_mode = parser.attribute_present("TargetMode")
            ? parser.attribute<xlnt::target_mode>("TargetMode")
            : xlnt::target_mode::internal;
        relationships.emplace_back(parser.attribute("Id"),
            parser.attribute<xlnt::relationship_type>("Type"),
            xlnt::uri(part.string()), xlnt::uri(parser.attribute("Target")),
            target_mode);

        expect_end_element(qn("relationships", "Relationship"));
    }

    expect_end_element(qn("relationships", "Relationships"));
    parser_ = nullptr;

    return relationships;
}

void xlsx_consumer::read_part(const std::vector<relationship> &rel_chain)
{
    const auto &manifest = target_.manifest();
    const auto part_path = manifest.canonicalize(rel_chain);
    auto part_streambuf = archive_->open(part_path);
    std::istream part_stream(part_streambuf.get());
    xml::parser parser(part_stream, part_path.string());
    parser_ = &parser;

    switch (rel_chain.back().type())
    {
    case relationship_type::core_properties:
        read_core_properties();
        break;

    case relationship_type::extended_properties:
        read_extended_properties();
        break;

    case relationship_type::custom_properties:
        read_custom_properties();
        break;

    case relationship_type::office_document:
        read_office_document(manifest.content_type(part_path));
        break;

    case relationship_type::connections:
        read_connections();
        break;

    case relationship_type::custom_xml_mappings:
        read_custom_xml_mappings();
        break;

    case relationship_type::external_workbook_references:
        read_external_workbook_references();
        break;

    case relationship_type::pivot_table:
        read_pivot_table();
        break;

    case relationship_type::shared_workbook_revision_headers:
        read_shared_workbook_revision_headers();
        break;

    case relationship_type::volatile_dependencies:
        read_volatile_dependencies();
        break;

    case relationship_type::shared_string_table:
        read_shared_string_table();
        break;

    case relationship_type::stylesheet:
        read_stylesheet();
        break;

    case relationship_type::theme:
        read_theme();
        break;

    case relationship_type::chartsheet:
        read_chartsheet(rel_chain.back().id());
        break;

    case relationship_type::dialogsheet:
        read_dialogsheet(rel_chain.back().id());
        break;

    case relationship_type::worksheet:
        read_worksheet(rel_chain.back().id());
        break;

    case relationship_type::thumbnail:
        read_image(part_path);
        break;

    case relationship_type::calculation_chain:
        read_calculation_chain();
        break;

    case relationship_type::hyperlink:
        break;

    case relationship_type::comments:
        break;

    case relationship_type::vml_drawing:
        break;

    case relationship_type::unknown:
        break;

    case relationship_type::printer_settings:
        break;

    case relationship_type::custom_property:
        break;

    case relationship_type::drawings:
        break;

    case relationship_type::pivot_table_cache_definition:
        break;

    case relationship_type::pivot_table_cache_records:
        break;

    case relationship_type::query_table:
        break;

    case relationship_type::shared_workbook:
        break;

    case relationship_type::revision_log:
        break;

    case relationship_type::shared_workbook_user_data:
        break;

    case relationship_type::single_cell_table_definitions:
        break;

    case relationship_type::table_definition:
        break;

    case relationship_type::image:
        read_image(part_path);
        break;
    }

    parser_ = nullptr;
}

void xlsx_consumer::populate_workbook()
{
    target_.clear();

    read_content_types();
    const auto root_path = path("/");

    for (const auto &package_rel : read_relationships(root_path))
    {
        manifest().register_relationship(package_rel);
    }

    for (auto package_rel : manifest().relationships(root_path))
    {
        if (package_rel.type() == relationship_type::office_document)
        {
            // Read the workbook after all the other package parts
            continue;
        }

        read_part({package_rel});
    }

    for (const auto &relationship_source_string : archive_->files())
    {
        for (const auto &part_rel : read_relationships(path(relationship_source_string)))
        {
            manifest().register_relationship(part_rel);
        }
    }

    read_part({ manifest().relationship(root_path,
        relationship_type::office_document) });
}

// Package Parts

void xlsx_consumer::read_content_types()
{
    auto &manifest = target_.manifest();
    auto content_types_streambuf = archive_->open(path("[Content_Types].xml"));
    std::istream content_types_stream(content_types_streambuf.get());
    xml::parser parser(content_types_stream, "[Content_Types].xml");
    parser_ = &parser;

    expect_start_element(qn("content-types", "Types"), xml::content::complex);

    while (in_element(qn("content-types", "Types")))
    {
        auto current_element = expect_start_element(xml::content::complex);

        if (current_element == qn("content-types", "Default"))
        {
            auto extension = parser.attribute("Extension");
            auto content_type = parser.attribute("ContentType");
            manifest.register_default_type(extension, content_type);
        }
        else if (current_element == qn("content-types", "Override"))
        {
            auto part_name = parser.attribute("PartName");
            auto content_type = parser.attribute("ContentType");
            manifest.register_override_type(path(part_name), content_type);
        }
        else
        {
            unexpected_element(current_element);
        }

        expect_end_element(current_element);
    }

    expect_end_element(qn("content-types", "Types"));
}

void xlsx_consumer::read_core_properties()
{
    //qn("extended-properties", "Properties");
    //qn("custom-properties", "Properties");
    expect_start_element(qn("core-properties", "coreProperties"), xml::content::complex);

    while (in_element(qn("core-properties", "coreProperties")))
    {
        const auto property_element = expect_start_element(xml::content::simple);
        const auto prop = detail::from_string<core_property>(property_element.name());
        if (prop == core_property::created || prop == core_property::modified)
        {
            skip_attribute(qn("xsi", "type"));
        }
        target_.core_property(prop, read_text());
        expect_end_element(property_element);
    }

    expect_end_element(qn("core-properties", "coreProperties"));
}

void xlsx_consumer::read_extended_properties()
{
    expect_start_element(qn("extended-properties", "Properties"), xml::content::complex);

    while (in_element(qn("extended-properties", "Properties")))
    {
        const auto property_element = expect_start_element(xml::content::mixed);
        const auto prop = detail::from_string<extended_property>(property_element.name());
        target_.extended_property(prop, read_variant());
        expect_end_element(property_element);
    }

    expect_end_element(qn("extended-properties", "Properties"));
}

void xlsx_consumer::read_custom_properties()
{
    expect_start_element(qn("custom-properties", "Properties"), xml::content::complex);

    while (in_element(qn("custom-properties", "Properties")))
    {
        const auto property_element = expect_start_element(xml::content::complex);
        const auto prop = parser().attribute("name");
        const auto format_id = parser().attribute("fmtid");
        const auto property_id = parser().attribute("pid");
        target_.custom_property(prop, read_variant());
        expect_end_element(property_element);
    }

    expect_end_element(qn("custom-properties", "Properties"));
}

void xlsx_consumer::read_office_document(const std::string &content_type) // CT_Workbook
{
    if (content_type != "application/vnd."
        "openxmlformats-officedocument.spreadsheetml.sheet.main+xml"
        && content_type != "application/vnd."
        "openxmlformats-officedocument.spreadsheetml.template.main+xml")
    {
        throw xlnt::invalid_file(content_type);
    }

    expect_start_element(qn("workbook", "workbook"), xml::content::complex);
    skip_attribute(qn("mc", "Ignorable"));
    read_namespaces();

    while (in_element(qn("workbook", "workbook")))
    {
        auto current_workbook_element = expect_start_element(xml::content::complex);

        if (current_workbook_element == qn("workbook", "fileVersion")) // CT_FileVersion 0-1
        {
            detail::workbook_impl::file_version_t file_version;

            if (parser().attribute_present("appName"))
            {
                file_version.app_name = parser().attribute("appName");
            }

            if (parser().attribute_present("lastEdited"))
            {
                file_version.last_edited = parser().attribute<std::size_t>("lastEdited");
            }

            if (parser().attribute_present("lowestEdited"))
            {
                file_version.lowest_edited = parser().attribute<std::size_t>("lowestEdited");
            }

            if (parser().attribute_present("lowestEdited"))
            {
                file_version.rup_build = parser().attribute<std::size_t>("rupBuild");
            }

            skip_attribute("codeName");

            target_.d_->file_version_ = file_version;
        }
        else if (current_workbook_element == qn("workbook", "fileSharing")) // CT_FileSharing 0-1
        {
            skip_remaining_content(current_workbook_element);
        }
        else if (current_workbook_element == qn("workbook", "workbookPr")) // CT_WorkbookPr 0-1
        {
            if (parser().attribute_present("date1904"))
            {
                target_.base_date(
                    is_true(parser().attribute("date1904")) ? calendar::mac_1904 : calendar::windows_1900);
            }

            skip_attributes({"codeName", "defaultThemeVersion",
                "backupFile", "showObjects", "filterPrivacy", "dateCompatibility"});
        }
        else if (current_workbook_element == qn("workbook", "workbookProtection")) // CT_WorkbookProtection 0-1
        {
            skip_remaining_content(current_workbook_element);
        }
        else if (current_workbook_element == qn("workbook", "bookViews")) // CT_BookViews 0-1
        {
            while (in_element(qn("workbook", "bookViews")))
            {
                expect_start_element(qn("workbook", "workbookView"), xml::content::simple);
                skip_attributes({"activeTab", "firstSheet",
                    "showHorizontalScroll", "showSheetTabs", "showVerticalScroll"});

                workbook_view view;
                view.x_window = parser().attribute<std::size_t>("xWindow");
                view.y_window = parser().attribute<std::size_t>("yWindow");
                view.window_width = parser().attribute<std::size_t>("windowWidth");
                view.window_height = parser().attribute<std::size_t>("windowHeight");

                if (parser().attribute_present("tabRatio"))
                {
                    view.tab_ratio = parser().attribute<std::size_t>("tabRatio");
                }

                target_.view(view);

                skip_attributes();
                expect_end_element(qn("workbook", "workbookView"));
            }
        }
        else if (current_workbook_element == qn("workbook", "sheets")) // CT_Sheets 1
        {
            std::size_t index = 0;

            while (in_element(qn("workbook", "sheets")))
            {
                expect_start_element(qn("spreadsheetml", "sheet"), xml::content::simple);

                auto title = parser().attribute("name");
                skip_attribute("state");

                sheet_title_index_map_[title] = index++;
                sheet_title_id_map_[title] = parser().attribute<std::size_t>("sheetId");
                target_.d_->sheet_title_rel_id_map_[title] = parser().attribute(qn("r", "id"));

                expect_end_element(qn("spreadsheetml", "sheet"));
            }
        }
        else if (current_workbook_element == qn("workbook", "functionGroups")) // CT_FunctionGroups 0-1
        {
            skip_remaining_content(current_workbook_element);
        }
        else if (current_workbook_element == qn("workbook", "externalReferences")) // CT_ExternalReferences 0-1
        {
            skip_remaining_content(current_workbook_element);
        }
        else if (current_workbook_element == qn("workbook", "definedNames")) // CT_DefinedNames 0-1
        {
            skip_remaining_content(current_workbook_element);
        }
        else if (current_workbook_element == qn("workbook", "calcPr")) // CT_CalcPr 0-1
        {
            skip_remaining_content(current_workbook_element);
        }
        else if (current_workbook_element == qn("workbook", "oleSize")) // CT_OleSize 0-1
        {
            skip_remaining_content(current_workbook_element);
        }
        else if (current_workbook_element == qn("workbook", "customWorkbookViews")) // CT_CustomWorkbookViews 0-1
        {
            skip_remaining_content(current_workbook_element);
        }
        else if (current_workbook_element == qn("workbook", "pivotCaches")) // CT_PivotCaches 0-1
        {
            skip_remaining_content(current_workbook_element);
        }
        else if (current_workbook_element == qn("workbook", "smartTagPr")) // CT_SmartTagPr 0-1
        {
            skip_remaining_content(current_workbook_element);
        }
        else if (current_workbook_element == qn("workbook", "smartTagTypes")) // CT_SmartTagTypes 0-1
        {
            skip_remaining_content(current_workbook_element);
        }
        else if (current_workbook_element == qn("workbook", "webPublishing")) // CT_WebPublishing 0-1
        {
            skip_remaining_content(current_workbook_element);
        }
        else if (current_workbook_element == qn("workbook", "fileRecoveryPr")) // CT_FileRecoveryPr 0+
        {
            skip_remaining_content(current_workbook_element);
        }
        else if (current_workbook_element == qn("workbook", "webPublishObjects")) // CT_WebPublishObjects 0-1
        {
            skip_remaining_content(current_workbook_element);
        }
        else if (current_workbook_element == qn("workbook", "extLst")) // CT_ExtensionList 0-1
        {
            skip_remaining_content(current_workbook_element);
        }
        else if (current_workbook_element == qn("mc", "AlternateContent"))
        {
            skip_remaining_content(current_workbook_element);
        }
        else
        {
            unexpected_element(current_workbook_element);
        }

        expect_end_element(current_workbook_element);
    }

    expect_end_element(qn("workbook", "workbook"));

    auto workbook_rel = manifest().relationship(path("/"), relationship_type::office_document);
    auto workbook_path = workbook_rel.target().path();

    if (manifest().has_relationship(workbook_path, relationship_type::shared_string_table))
    {
        read_part({workbook_rel, manifest().relationship(workbook_path, relationship_type::shared_string_table)});
    }

    if (manifest().has_relationship(workbook_path, relationship_type::stylesheet))
    {
        read_part({workbook_rel, manifest().relationship(workbook_path, relationship_type::stylesheet)});
    }

    if (manifest().has_relationship(workbook_path, relationship_type::theme))
    {
        read_part({workbook_rel, manifest().relationship(workbook_path, relationship_type::theme)});
    }

    for (auto worksheet_rel : manifest().relationships(workbook_path, relationship_type::worksheet))
    {
        read_part({workbook_rel, worksheet_rel});
    }
}

// Write Workbook Relationship Target Parts

void xlsx_consumer::read_calculation_chain()
{
}

void xlsx_consumer::read_chartsheet(const std::string & /*title*/)
{
}

void xlsx_consumer::read_connections()
{
}

void xlsx_consumer::read_custom_property()
{
}

void xlsx_consumer::read_custom_xml_mappings()
{
}

void xlsx_consumer::read_dialogsheet(const std::string & /*title*/)
{
}

void xlsx_consumer::read_external_workbook_references()
{
}

void xlsx_consumer::read_pivot_table()
{
}

void xlsx_consumer::read_shared_string_table()
{
    expect_start_element(qn("spreadsheetml", "sst"), xml::content::complex);
    skip_attributes({"count"});

    bool has_unique_count = false;
    std::size_t unique_count = 0;

    if (parser().attribute_present("uniqueCount"))
    {
        has_unique_count = true;
        unique_count = parser().attribute<std::size_t>("uniqueCount");
    }

    auto &strings = target_.shared_strings();

    while (in_element(qn("spreadsheetml", "sst")))
    {
        expect_start_element(qn("spreadsheetml", "si"), xml::content::complex);
        strings.push_back(read_rich_text(qn("spreadsheetml", "si")));
        expect_end_element(qn("spreadsheetml", "si"));
    }

    expect_end_element(qn("spreadsheetml", "sst"));

    if (has_unique_count && unique_count != strings.size())
    {
        throw invalid_file("sizes don't match");
    }
}

void xlsx_consumer::read_shared_workbook_revision_headers()
{
}

void xlsx_consumer::read_shared_workbook()
{
}

void xlsx_consumer::read_shared_workbook_user_data()
{
}

void xlsx_consumer::read_stylesheet()
{
    target_.impl().stylesheet_ = detail::stylesheet();
    auto &stylesheet = target_.impl().stylesheet_.get();

    expect_start_element(qn("spreadsheetml", "styleSheet"), xml::content::complex);
    skip_attributes({qn("mc", "Ignorable")});
    read_namespaces();

    std::vector<std::pair<style_impl, std::size_t>> styles;
    std::vector<std::pair<format_impl, std::size_t>> format_records;
    std::vector<std::pair<format_impl, std::size_t>> style_records;

    while (in_element(qn("spreadsheetml", "styleSheet")))
    {
        auto current_style_element = expect_start_element(xml::content::complex);

        if (current_style_element == qn("spreadsheetml", "borders"))
        {
            auto &borders = stylesheet.borders;
            auto count = parser().attribute<std::size_t>("count");

            while (in_element(qn("spreadsheetml", "borders")))
            {
                borders.push_back(xlnt::border());
                auto &border = borders.back();

                expect_start_element(qn("spreadsheetml", "border"), xml::content::complex);

                auto diagonal = diagonal_direction::neither;

                if (parser().attribute_present("diagonalDown") && parser().attribute("diagonalDown") == "1")
                {
                    diagonal = diagonal_direction::down;
                }

                if (parser().attribute_present("diagonalUp") && parser().attribute("diagonalUp") == "1")
                {
                    diagonal = diagonal == diagonal_direction::down ? diagonal_direction::both : diagonal_direction::up;
                }

                if (diagonal != diagonal_direction::neither)
                {
                    border.diagonal(diagonal);
                }

                while (in_element(qn("spreadsheetml", "border")))
                {
                    auto current_side_element = expect_start_element(xml::content::complex);

                    xlnt::border::border_property side;

                    if (parser().attribute_present("style"))
                    {
                        side.style(parser().attribute<xlnt::border_style>("style"));
                    }

                    if (in_element(current_side_element))
                    {
                        expect_start_element(qn("spreadsheetml", "color"), xml::content::complex);
                        side.color(read_color());
                        expect_end_element(qn("spreadsheetml", "color"));
                    }

                    expect_end_element(current_side_element);

                    auto side_type = xml::value_traits<xlnt::border_side>::parse(current_side_element.name(), parser());
                    border.side(side_type, side);
                }

                expect_end_element(qn("spreadsheetml", "border"));
            }

            if (count != borders.size())
            {
                throw xlnt::exception("border counts don't match");
            }
        }
        else if (current_style_element == qn("spreadsheetml", "fills"))
        {
            auto &fills = stylesheet.fills;
            auto count = parser().attribute<std::size_t>("count");

            while (in_element(qn("spreadsheetml", "fills")))
            {
                fills.push_back(xlnt::fill());
                auto &new_fill = fills.back();

                expect_start_element(qn("spreadsheetml", "fill"), xml::content::complex);
                auto fill_element = expect_start_element(xml::content::complex);

                if (fill_element == qn("spreadsheetml", "patternFill"))
                {
                    xlnt::pattern_fill pattern;

                    if (parser().attribute_present("patternType"))
                    {
                        pattern.type(parser().attribute<xlnt::pattern_fill_type>("patternType"));

                        while (in_element(qn("spreadsheetml", "patternFill")))
                        {
                            auto pattern_type_element = expect_start_element(xml::content::complex);

                            if (pattern_type_element == qn("spreadsheetml", "fgColor"))
                            {
                                pattern.foreground(read_color());
                            }
                            else if (pattern_type_element == qn("spreadsheetml", "bgColor"))
                            {
                                pattern.background(read_color());
                            }
                            else
                            {
                                unexpected_element(pattern_type_element);
                            }

                            expect_end_element(pattern_type_element);
                        }
                    }

                    new_fill = pattern;
                }
                else if (fill_element == qn("spreadsheetml", "gradientFill"))
                {
                    xlnt::gradient_fill gradient;

                    if (parser().attribute_present("type"))
                    {
                        gradient.type(parser().attribute<xlnt::gradient_fill_type>("type"));
                    }
                    else
                    {
                        gradient.type(xlnt::gradient_fill_type::linear);
                    }

                    while (in_element(qn("spreadsheetml", "gradientFill")))
                    {
                        expect_start_element(qn("spreadsheetml", "stop"), xml::content::complex);
                        auto position = parser().attribute<double>("position");
                        expect_start_element(qn("spreadsheetml", "color"), xml::content::complex);
                        auto color = read_color();
                        expect_end_element(qn("spreadsheetml", "color"));
                        expect_end_element(qn("spreadsheetml", "stop"));

                        gradient.add_stop(position, color);
                    }

                    new_fill = gradient;
                }
                else
                {
                    unexpected_element(fill_element);
                }

                expect_end_element(fill_element);
                expect_end_element(qn("spreadsheetml", "fill"));
            }

            if (count != fills.size())
            {
                throw xlnt::exception("counts don't match");
            }
        }
        else if (current_style_element == qn("spreadsheetml", "fonts"))
        {
            auto &fonts = stylesheet.fonts;
            auto count = parser().attribute<std::size_t>("count");
            skip_attributes({qn("x14ac", "knownFonts")});

            while (in_element(qn("spreadsheetml", "fonts")))
            {
                fonts.push_back(xlnt::font());
                auto &new_font = stylesheet.fonts.back();

                expect_start_element(qn("spreadsheetml", "font"), xml::content::complex);

                while (in_element(qn("spreadsheetml", "font")))
                {
                    auto font_property_element = expect_start_element(xml::content::simple);

                    if (font_property_element == qn("spreadsheetml", "sz"))
                    {
                        new_font.size(parser().attribute<double>("val"));
                    }
                    else if (font_property_element == qn("spreadsheetml", "name"))
                    {
                        new_font.name(parser().attribute("val"));
                    }
                    else if (font_property_element == qn("spreadsheetml", "color"))
                    {
                        new_font.color(read_color());
                    }
                    else if (font_property_element == qn("spreadsheetml", "family"))
                    {
                        new_font.family(parser().attribute<std::size_t>("val"));
                    }
                    else if (font_property_element == qn("spreadsheetml", "scheme"))
                    {
                        new_font.scheme(parser().attribute("val"));
                    }
                    else if (font_property_element == qn("spreadsheetml", "b"))
                    {
                        if (parser().attribute_present("val"))
                        {
                            new_font.bold(is_true(parser().attribute("val")));
                        }
                        else
                        {
                            new_font.bold(true);
                        }
                    }
                    else if (font_property_element == qn("spreadsheetml", "vertAlign"))
                    {
                        new_font.superscript(parser().attribute("val") == "superscript");
                    }
                    else if (font_property_element == qn("spreadsheetml", "strike"))
                    {
                        if (parser().attribute_present("val"))
                        {
                            new_font.strikethrough(is_true(parser().attribute("val")));
                        }
                        else
                        {
                            new_font.strikethrough(true);
                        }
                    }
                    else if (font_property_element == qn("spreadsheetml", "outline"))
                    {
                        if (parser().attribute_present("val"))
                        {
                            new_font.outline(is_true(parser().attribute("val")));
                        }
                        else
                        {
                            new_font.outline(true);
                        }
                    }
                    else if (font_property_element == qn("spreadsheetml", "shadow"))
                    {
                        if (parser().attribute_present("val"))
                        {
                            new_font.shadow(is_true(parser().attribute("val")));
                        }
                        else
                        {
                            new_font.shadow(true);
                        }
                    }
                    else if (font_property_element == qn("spreadsheetml", "i"))
                    {
                        if (parser().attribute_present("val"))
                        {
                            new_font.italic(is_true(parser().attribute("val")));
                        }
                        else
                        {
                            new_font.italic(true);
                        }
                    }
                    else if (font_property_element == qn("spreadsheetml", "u"))
                    {
                        if (parser().attribute_present("val"))
                        {
                            new_font.underline(parser().attribute<xlnt::font::underline_style>("val"));
                        }
                        else
                        {
                            new_font.underline(xlnt::font::underline_style::single);
                        }
                    }
                    else if (font_property_element == qn("spreadsheetml", "charset"))
                    {
                        if (parser().attribute_present("val"))
                        {
                            parser().attribute("val");
                        }
                    }
                    else
                    {
                        unexpected_element(font_property_element);
                    }

                    expect_end_element(font_property_element);
                }

                expect_end_element(qn("spreadsheetml", "font"));
            }

            if (count != stylesheet.fonts.size())
            {
                throw xlnt::exception("counts don't match");
            }
        }
        else if (current_style_element == qn("spreadsheetml", "numFmts"))
        {
            auto &number_formats = stylesheet.number_formats;
            auto count = parser().attribute<std::size_t>("count");

            while (in_element(qn("spreadsheetml", "numFmts")))
            {
                expect_start_element(qn("spreadsheetml", "numFmt"), xml::content::simple);

                auto format_string = parser().attribute("formatCode");

                if (format_string == "GENERAL")
                {
                    format_string = "General";
                }

                xlnt::number_format nf;

                nf.format_string(format_string);
                nf.id(parser().attribute<std::size_t>("numFmtId"));

                expect_end_element(qn("spreadsheetml", "numFmt"));

                number_formats.push_back(nf);
            }

            if (count != number_formats.size())
            {
                throw xlnt::exception("counts don't match");
            }
        }
        else if (current_style_element == qn("spreadsheetml", "cellStyles"))
        {
            auto count = parser().attribute<std::size_t>("count");

            while (in_element(qn("spreadsheetml", "cellStyles")))
            {
                auto &data = *styles.emplace(styles.end());

                expect_start_element(qn("spreadsheetml", "cellStyle"), xml::content::simple);

                data.first.name = parser().attribute("name");
                data.second = parser().attribute<std::size_t>("xfId");

                if (parser().attribute_present("builtinId"))
                {
                    data.first.builtin_id = parser().attribute<std::size_t>("builtinId");
                }

                if (parser().attribute_present("hidden"))
                {
                    data.first.hidden_style = is_true(parser().attribute("hidden"));
                }

                if (parser().attribute_present("customBuiltin"))
                {
                    data.first.custom_builtin = is_true(parser().attribute("customBuiltin"));
                }

                expect_end_element(qn("spreadsheetml", "cellStyle"));
            }

            if (count != styles.size())
            {
                throw xlnt::exception("counts don't match");
            }
        }
        else if (current_style_element == qn("spreadsheetml", "cellStyleXfs")
            || current_style_element == qn("spreadsheetml", "cellXfs"))
        {
            auto in_style_records = current_style_element.name() == "cellStyleXfs";
            auto count = parser().attribute<std::size_t>("count");

            while (in_element(current_style_element))
            {
                expect_start_element(qn("spreadsheetml", "xf"), xml::content::complex);

                auto &record = *(!in_style_records
                    ? format_records.emplace(format_records.end())
                    : style_records.emplace(style_records.end()));

                record.first.border_applied = parser().attribute_present("applyBorder")
                    && is_true(parser().attribute("applyBorder"));
                record.first.border_id = parser().attribute_present("borderId")
                    ? parser().attribute<std::size_t>("borderId") : 0;

                record.first.fill_applied = parser().attribute_present("applyFill")
                    && is_true(parser().attribute("applyFill"));
                record.first.fill_id = parser().attribute_present("fillId")
                    ? parser().attribute<std::size_t>("fillId") : 0;

                record.first.font_applied = parser().attribute_present("applyFont")
                    && is_true(parser().attribute("applyFont"));
                record.first.font_id = parser().attribute_present("fontId")
                    ? parser().attribute<std::size_t>("fontId") : 0;

                record.first.number_format_applied = parser().attribute_present("applyNumberFormat")
                    && is_true(parser().attribute("applyNumberFormat"));
                record.first.number_format_id = parser().attribute_present("numFmtId")
                    ? parser().attribute<std::size_t>("numFmtId") : 0;

                auto apply_alignment_present = parser().attribute_present("applyAlignment");
                record.first.alignment_applied = apply_alignment_present
                    && is_true(parser().attribute("applyAlignment"));

                auto apply_protection_present = parser().attribute_present("applyProtection");
                record.first.protection_applied = apply_protection_present
                    && is_true(parser().attribute("applyProtection"));

                record.first.pivot_button_ = parser().attribute_present("pivotButton")
                    && is_true(parser().attribute("pivotButton"));
                record.first.quote_prefix_ = parser().attribute_present("quotePrefix")
                    && is_true(parser().attribute("quotePrefix"));

                if (parser().attribute_present("xfId") && parser().name() == "cellXfs")
                {
                    record.second = parser().attribute<std::size_t>("xfId");
                }

                while (in_element(qn("spreadsheetml", "xf")))
                {
                    auto xf_child_element = expect_start_element(xml::content::simple);

                    if (xf_child_element == qn("spreadsheetml", "alignment"))
                    {
                        record.first.alignment_id = stylesheet.alignments.size();
                        auto &alignment = *stylesheet.alignments.emplace(stylesheet.alignments.end());

                        if (parser().attribute_present("wrapText"))
                        {
                            alignment.wrap(is_true(parser().attribute("wrapText")));
                        }

                        if (parser().attribute_present("shrinkToFit"))
                        {
                            alignment.shrink(is_true(parser().attribute("shrinkToFit")));
                        }

                        if (parser().attribute_present("indent"))
                        {
                            alignment.indent(parser().attribute<int>("indent"));
                        }

                        if (parser().attribute_present("textRotation"))
                        {
                            alignment.rotation(parser().attribute<int>("textRotation"));
                        }

                        if (parser().attribute_present("vertical"))
                        {
                            alignment.vertical(parser().attribute<xlnt::vertical_alignment>("vertical"));
                        }

                        if (parser().attribute_present("horizontal"))
                        {
                            alignment.horizontal(parser().attribute<xlnt::horizontal_alignment>("horizontal"));
                        }
                    }
                    else if (xf_child_element == qn("spreadsheetml", "protection"))
                    {
                        record.first.protection_id = stylesheet.protections.size();
                        auto &protection = *stylesheet.protections.emplace(stylesheet.protections.end());

                        protection.locked(parser().attribute_present("locked")
                            && is_true(parser().attribute("locked")));
                        protection.hidden(parser().attribute_present("hidden")
                            && is_true(parser().attribute("hidden")));
                    }
                    else
                    {
                        unexpected_element(xf_child_element);
                    }

                    expect_end_element(xf_child_element);
                }

                expect_end_element(qn("spreadsheetml", "xf"));
            }

            if ((in_style_records && count != style_records.size())
                || (!in_style_records && count != format_records.size()))
            {
                throw xlnt::exception("counts don't match");
            }
        }
        else if (current_style_element == qn("spreadsheetml", "dxfs"))
        {
            auto count = parser().attribute<std::size_t>("count");
            std::size_t processed = 0;

            while (in_element(current_style_element))
            {
                auto current_element = expect_start_element(xml::content::mixed);
                skip_remaining_content(current_element);
                expect_end_element(current_element);
                ++processed;
            }

            if (count != processed)
            {
                throw xlnt::exception("counts don't match");
            }
        }
        else if (current_style_element == qn("spreadsheetml", "tableStyles"))
        {
            skip_attribute("defaultTableStyle");
            skip_attribute("defaultPivotStyle");

            auto count = parser().attribute<std::size_t>("count");
            std::size_t processed = 0;

            while (in_element(qn("spreadsheetml", "tableStyles")))
            {
                auto current_element = expect_start_element(xml::content::complex);
                skip_remaining_content(current_element);
                expect_end_element(current_element);
                ++processed;
            }

            if (count != processed)
            {
                throw xlnt::exception("counts don't match");
            }
        }
        else if (current_style_element == qn("spreadsheetml", "extLst"))
        {
            skip_remaining_content(current_style_element);
        }
        else if (current_style_element == qn("spreadsheetml", "colors")) // CT_Colors 0-1
        {
            while (in_element(qn("spreadsheetml", "colors")))
            {
                auto colors_child_element = expect_start_element(xml::content::complex);

                if (colors_child_element == qn("spreadsheetml", "indexedColors")) // CT_IndexedColors 0-1
                {
                    while (in_element(colors_child_element))
                    {
                        expect_start_element(qn("spreadsheetml", "rgbColor"), xml::content::simple);
                        stylesheet.colors.push_back(read_color());
                        expect_end_element(qn("spreadsheetml", "rgbColor"));
                    }
                }
                else if (colors_child_element == qn("spreadsheetml", "mruColors")) // CT_MRUColors
                {
                    skip_remaining_content(colors_child_element);
                }
                else
                {
                    unexpected_element(colors_child_element);
                }

                expect_end_element(colors_child_element);
            }
        }
        else
        {
            unexpected_element(current_style_element);
        }

        expect_end_element(current_style_element);
    }

    expect_end_element(qn("spreadsheetml", "styleSheet"));
/*
    auto lookup_number_format = [&](std::size_t number_format_id) {
        auto result = number_format::general();
        bool is_custom_number_format = false;

        for (const auto &nf : stylesheet.number_formats)
        {
            if (nf.id() == number_format_id)
            {
                result = nf;
                is_custom_number_format = true;
                break;
            }
        }

        if (number_format_id < 164 && !is_custom_number_format)
        {
            result = number_format::from_builtin_id(number_format_id);
        }

        return result;
    };
*/
    /*
    std::size_t xf_id = 0;

    for (const auto &record : style_records)
    {
        auto style_iter = std::find_if(styles.begin(), styles.end(),
            [&xf_id](const std::pair<style_impl, std::size_t> &s) { return s.second == xf_id; });
        ++xf_id;

        if (style_iter == styles.end()) continue;

        auto new_style = stylesheet.create_style(style_iter->first.name);
        *new_style.d_ = style_iter->first;

        (void)record;
    }
    */
    std::size_t record_index = 0;

    for (const auto &record : format_records)
    {
        stylesheet.format_impls.push_back(format_impl());
        auto &new_format = stylesheet.format_impls.back();

        new_format.id = record_index++;
        new_format.parent = &stylesheet;

        ++new_format.references;

        new_format.alignment_id = record.first.alignment_id;
        new_format.alignment_applied = record.first.alignment_applied;
        new_format.border_id = record.first.border_id;
        new_format.border_applied = record.first.border_applied;
        new_format.fill_id = record.first.fill_id;
        new_format.fill_applied = record.first.fill_applied;
        new_format.font_id = record.first.font_id;
        new_format.font_applied = record.first.font_applied;
        new_format.number_format_id = record.first.number_format_id;
        new_format.number_format_applied = record.first.number_format_applied;
        new_format.protection_id = record.first.protection_id;
        new_format.protection_applied = record.first.protection_applied;
        new_format.pivot_button_ = record.first.pivot_button_;
        new_format.quote_prefix_ = record.first.quote_prefix_;
    }
}

void xlsx_consumer::read_theme()
{
    auto workbook_rel = manifest().relationship(path("/"), relationship_type::office_document);
    auto theme_rel = manifest().relationship(workbook_rel.target().path(), relationship_type::theme);
    auto theme_path = manifest().canonicalize({workbook_rel, theme_rel});

    target_.theme(theme());

    if (manifest().has_relationship(theme_path, relationship_type::image))
    {
        read_part({workbook_rel, theme_rel, manifest().relationship(theme_path, relationship_type::image)});
    }
}

void xlsx_consumer::read_volatile_dependencies()
{
}

// CT_Worksheet
void xlsx_consumer::read_worksheet(const std::string &rel_id)
{
/*
    static const auto &xmlns = constants::namespace_("spreadsheetml");
    static const auto &xmlns_mc = constants::namespace_("mc");
    static const auto &xmlns_x14ac = constants::namespace_("x14ac");
    static const auto &xmlns_r = constants::namespace_("r");
*/
    auto title = std::find_if(target_.d_->sheet_title_rel_id_map_.begin(),
        target_.d_->sheet_title_rel_id_map_.end(),
        [&](const std::pair<std::string, std::string> &p) {
            return p.second == rel_id;
        })->first;

    auto id = sheet_title_id_map_[title];
    auto index = sheet_title_index_map_[title];

    auto insertion_iter = target_.d_->worksheets_.begin();
    while (insertion_iter != target_.d_->worksheets_.end() && sheet_title_index_map_[insertion_iter->title_] < index)
    {
        ++insertion_iter;
    }

    target_.d_->worksheets_.emplace(insertion_iter, &target_, id, title);

    auto ws = target_.sheet_by_id(id);

    expect_start_element(qn("spreadsheetml", "worksheet"), xml::content::complex); // CT_Worksheet
    skip_attributes({qn("mc", "Ignorable")});
    read_namespaces();

    xlnt::range_reference full_range;

    const auto &shared_strings = target_.shared_strings();
    auto &manifest = target_.manifest();

    const auto workbook_rel = manifest.relationship(path("/"), relationship_type::office_document);
    const auto sheet_rel = manifest.relationship(workbook_rel.target().path(), rel_id);
    path sheet_path(sheet_rel.source().path().parent().append(sheet_rel.target().path()));
    auto hyperlinks = manifest.relationships(sheet_path, xlnt::relationship_type::hyperlink);

    while (in_element(qn("spreadsheetml", "worksheet")))
    {
        auto current_worksheet_element = expect_start_element(xml::content::complex);

        if (current_worksheet_element == qn("spreadsheetml", "sheetPr")) // CT_SheetPr 0-1
        {
            while (in_element(current_worksheet_element))
            {
                auto sheet_pr_child_element = expect_start_element(xml::content::simple);

                if (sheet_pr_child_element == qn("spreadsheetml", "tabColor")) // CT_Color 0-1
                {
                    read_color();
                }
                else if (sheet_pr_child_element == qn("spreadsheetml", "outlinePr")) // CT_OutlinePr 0-1
                {
                    skip_attribute("applyStyles"); // optional, boolean, false
                    skip_attribute("summaryBelow"); // optional, boolean, true
                    skip_attribute("summaryRight"); // optional, boolean, true
                    skip_attribute("showOutlineSymbols"); // optional, boolean, true
                }
                else if (sheet_pr_child_element == qn("spreadsheetml", "pageSetUpPr")) // CT_PageSetUpPr 0-1
                {
                    skip_attribute("autoPageBreaks"); // optional, boolean, true
                    skip_attribute("fitToPage"); // optional, boolean, false
                }
                else
                {
                    unexpected_element(sheet_pr_child_element);
                }

                expect_end_element(sheet_pr_child_element);
            }

            skip_attribute("syncHorizontal"); // optional, boolean, false
            skip_attribute("syncVertical"); // optional, boolean, false
            skip_attribute("syncRef"); // optional, ST_Ref, false
            skip_attribute("transitionEvaluation"); // optional, boolean, false
            skip_attribute("transitionEntry"); // optional, boolean, false
            skip_attribute("published"); // optional, boolean, true
            skip_attribute("codeName"); // optional, string
            skip_attribute("filterMode"); // optional, boolean, false
            skip_attribute("enableFormatConditionsCalculation"); // optional, boolean, true
        }
        else if (current_worksheet_element == qn("spreadsheetml", "dimension")) // CT_SheetDimension 0-1
        {
            full_range = xlnt::range_reference(parser().attribute("ref"));
        }
        else if (current_worksheet_element == qn("spreadsheetml", "sheetViews")) // CT_SheetViews 0-1
        {
            while (in_element(current_worksheet_element))
            {
                expect_start_element(qn("spreadsheetml", "sheetView"), xml::content::complex); // CT_SheetView 1+

                sheet_view new_view;
                new_view.id(parser().attribute<std::size_t>("workbookViewId"));

                if (parser().attribute_present("showGridLines")) // default="true"
                {
                    new_view.show_grid_lines(is_true(parser().attribute("showGridLines")));
                }

                if (parser().attribute_present("defaultGridColor")) // default="true"
                {
                    new_view.default_grid_color(is_true(parser().attribute("defaultGridColor")));
                }

                if (parser().attribute_present("view") && parser().attribute("view") != "normal")
                {
                    new_view.type(parser().attribute("view") == "pageBreakPreview" ? sheet_view_type::page_break_preview
                                                                                   : sheet_view_type::page_layout);
                }

                skip_attributes({"windowProtection", "showFormulas", "showRowColHeaders", "showZeros", "rightToLeft",
                    "tabSelected", "showRuler", "showOutlineSymbols", "showWhiteSpace", "view", "topLeftCell",
                    "colorId", "zoomScale", "zoomScaleNormal", "zoomScaleSheetLayoutView", "zoomScalePageLayoutView"});

                while (in_element(qn("spreadsheetml", "sheetView")))
                {
                    auto sheet_view_child_element = expect_start_element(xml::content::simple);

                    if (sheet_view_child_element == qn("spreadsheetml", "pane")) // CT_Pane 0-1
                    {
                        pane new_pane;

                        if (parser().attribute_present("topLeftCell"))
                        {
                            new_pane.top_left_cell = cell_reference(parser().attribute("topLeftCell"));
                        }

                        if (parser().attribute_present("xSplit"))
                        {
                            new_pane.x_split = parser().attribute<column_t::index_t>("xSplit");
                        }

                        if (parser().attribute_present("ySplit"))
                        {
                            new_pane.y_split = parser().attribute<row_t>("ySplit");
                        }

                        if (parser().attribute_present("activePane"))
                        {
                            new_pane.active_pane = parser().attribute<pane_corner>("activePane");
                        }

                        if (parser().attribute_present("state"))
                        {
                            new_pane.state = parser().attribute<pane_state>("state");
                        }

                        new_view.pane(new_pane);
                    }
                    else if (sheet_view_child_element == qn("spreadsheetml", "selection")) // CT_Selection 0-4
                    {
                        skip_remaining_content(sheet_view_child_element);
                    }
                    else if (sheet_view_child_element == qn("spreadsheetml", "pivotSelection")) // CT_PivotSelection 0-4
                    {
                        skip_remaining_content(sheet_view_child_element);
                    }
                    else if (sheet_view_child_element == qn("spreadsheetml", "extLst")) // CT_ExtensionList 0-1
                    {
                        skip_remaining_content(sheet_view_child_element);
                    }
                    else
                    {
                        unexpected_element(sheet_view_child_element);
                    }

                    expect_end_element(sheet_view_child_element);
                }

                expect_end_element(qn("spreadsheetml", "sheetView"));

                ws.d_->views_.push_back(new_view);
            }
        }
        else if (current_worksheet_element == qn("spreadsheetml", "sheetFormatPr")) // CT_SheetFormatPr 0-1
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "cols")) // CT_Cols 0+
        {
            while (in_element(qn("spreadsheetml", "cols")))
            {
                expect_start_element(qn("spreadsheetml", "col"), xml::content::simple);

                skip_attributes({"bestFit", "collapsed", "outlineLevel"});

                auto min = static_cast<column_t::index_t>(std::stoull(parser().attribute("min")));
                auto max = static_cast<column_t::index_t>(std::stoull(parser().attribute("max")));

                optional<double> width;

                if (parser().attribute_present("width"))
                {
                    width = parser().attribute<double>("width");
                }

                optional<std::size_t> column_style;

                if (parser().attribute_present("style"))
                {
                    column_style = parser().attribute<std::size_t>("style");
                }

                auto custom =
                    parser().attribute_present("customWidth") ? is_true(parser().attribute("customWidth")) : false;
                auto hidden = parser().attribute_present("hidden") ? is_true(parser().attribute("hidden")) : false;

                expect_end_element(qn("spreadsheetml", "col"));

                for (auto column = min; column <= max; column++)
                {
                    column_properties props;

                    if (width.is_set())
                    {
                        props.width = width.get();
                    }

                    if (column_style.is_set())
                    {
                        props.style = column_style.get();
                    }

                    props.hidden = hidden;
                    props.custom_width = custom;
                    ws.add_column_properties(column, props);
                }
            }
        }
        else if (current_worksheet_element == qn("spreadsheetml", "sheetData")) // CT_SheetData 1
        {
            while (in_element(qn("spreadsheetml", "sheetData")))
            {
                expect_start_element(qn("spreadsheetml", "row"), xml::content::complex); // CT_Row
                auto row_index = parser().attribute<row_t>("r");

                if (parser().attribute_present("ht"))
                {
                    ws.row_properties(row_index).height = parser().attribute<double>("ht");
                }

                if (parser().attribute_present("customHeight"))
                {
                    ws.row_properties(row_index).custom_height = is_true(parser().attribute("customHeight"));
                }

                if (parser().attribute_present("hidden") && is_true(parser().attribute("hidden")))
                {
                    ws.row_properties(row_index).hidden = true;
                }

                skip_attributes({qn("x14ac", "dyDescent")});
                skip_attributes({"customFormat", "s", "customFont",
                    "outlineLevel", "collapsed", "thickTop", "thickBot",
                    "ph", "spans"});

                while (in_element(qn("spreadsheetml", "row")))
                {
                    expect_start_element(qn("spreadsheetml", "c"), xml::content::complex);
                    auto cell = ws.cell(cell_reference(parser().attribute("r")));

                    auto has_type = parser().attribute_present("t");
                    auto type = has_type ? parser().attribute("t") : "n";

                    auto has_format = parser().attribute_present("s");
                    auto format_id = static_cast<std::size_t>(has_format ? std::stoull(parser().attribute("s")) : 0LL);

                    auto has_value = false;
                    auto value_string = std::string();

                    auto has_formula = false;
                    auto has_shared_formula = false;
                    auto formula_value_string = std::string();

                    while (in_element(qn("spreadsheetml", "c")))
                    {
                        auto current_element = expect_start_element(xml::content::mixed);

                        if (current_element == qn("spreadsheetml", "v")) // s:ST_Xstring
                        {
                            has_value = true;
                            value_string = read_text();
                        }
                        else if (current_element == qn("spreadsheetml", "f")) // CT_CellFormula
                        {
                            has_formula = true;

                            if (parser().attribute_present("t"))
                            {
                                has_shared_formula = parser().attribute("t") == "shared";
                            }

                            skip_attributes(
                                {"aca", "ref", "dt2D", "dtr", "del1", "del2", "r1", "r2", "ca", "si", "bx"});

                            formula_value_string = read_text();
                        }
                        else if (current_element == qn("spreadsheetml", "is")) // CT_Rst
                        {
                            expect_start_element(qn("spreadsheetml", "t"), xml::content::simple);
                            value_string = read_text();
                            expect_end_element(qn("spreadsheetml", "t"));
                        }
                        else
                        {
                            unexpected_element(current_element);
                        }

                        expect_end_element(current_element);
                    }

                    expect_end_element(qn("spreadsheetml", "c"));

                    if (has_formula && !has_shared_formula)
                    {
                        cell.formula(formula_value_string);
                    }

                    if (has_value)
                    {
                        if (type == "inlineStr" || type == "str")
                        {
                            cell.value(value_string);
                        }
                        else if (type == "s" && !has_formula)
                        {
                            auto shared_string_index = static_cast<std::size_t>(std::stoull(value_string));
                            auto shared_string = shared_strings.at(shared_string_index);
                            cell.value(shared_string);
                        }
                        else if (type == "b") // boolean
                        {
                            cell.value(is_true(value_string));
                        }
                        else if (type == "n") // numeric
                        {
                            cell.value(std::stold(value_string));
                        }
                        else if (!value_string.empty() && value_string[0] == '#')
                        {
                            cell.error(value_string);
                        }
                    }

                    if (has_format)
                    {
                        cell.format(target_.format(format_id));
                    }
                }

                expect_end_element(qn("spreadsheetml", "row"));
            }
        }
        else if (current_worksheet_element == qn("spreadsheetml", "sheetCalcPr")) // CT_SheetCalcPr 0-1
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "sheetProtection")) // CT_SheetProtection 0-1
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "protectedRanges")) // CT_ProtectedRanges 0-1
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "scenarios")) // CT_Scenarios 0-1
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "autoFilter")) // CT_AutoFilter 0-1
        {
            ws.auto_filter(xlnt::range_reference(parser().attribute("ref")));
            // auto filter complex
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "sortState")) // CT_SortState 0-1
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "dataConsolidate")) // CT_DataConsolidate 0-1
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "customSheetViews")) // CT_CustomSheetViews 0-1
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "mergeCells")) // CT_MergeCells 0-1
        {
            auto count = std::stoull(parser().attribute("count"));

            while (in_element(qn("spreadsheetml", "mergeCells")))
            {
                expect_start_element(qn("spreadsheetml", "mergeCell"), xml::content::simple);
                ws.merge_cells(range_reference(parser().attribute("ref")));
                expect_end_element(qn("spreadsheetml", "mergeCell"));

                count--;
            }

            if (count != 0)
            {
                throw invalid_file("sizes don't match");
            }
        }
        else if (current_worksheet_element == qn("spreadsheetml", "phoneticPr")) // CT_PhoneticPr 0-1
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "conditionalFormatting")) // CT_ConditionalFormatting 0+
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "dataValidations")) // CT_DataValidations 0-1
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "hyperlinks")) // CT_Hyperlinks 0-1
        {
            while (in_element(qn("spreadsheetml", "hyperlinks")))
            {
                expect_start_element(qn("spreadsheetml", "hyperlink"), xml::content::simple);

                auto cell = ws.cell(parser().attribute("ref"));

                if (parser().attribute_present(qn("r", "id")))
                {
                    auto hyperlink_rel_id = parser().attribute(qn("r", "id"));
                    auto hyperlink_rel = std::find_if(hyperlinks.begin(), hyperlinks.end(),
                        [&](const relationship &r) { return r.id() == hyperlink_rel_id; });

                    if (hyperlink_rel != hyperlinks.end())
                    {
                        cell.hyperlink(hyperlink_rel->target().path().string());
                    }
                }

                skip_attributes({"location", "tooltip", "display"});
                expect_end_element(qn("spreadsheetml", "hyperlink"));
            }
        }
        else if (current_worksheet_element == qn("spreadsheetml", "printOptions")) // CT_PrintOptions 0-1
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "pageMargins")) // CT_PageMargins 0-1
        {
            page_margins margins;

            margins.top(parser().attribute<double>("top"));
            margins.bottom(parser().attribute<double>("bottom"));
            margins.left(parser().attribute<double>("left"));
            margins.right(parser().attribute<double>("right"));
            margins.header(parser().attribute<double>("header"));
            margins.footer(parser().attribute<double>("footer"));

            ws.page_margins(margins);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "pageSetup")) // CT_PageSetup 0-1
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "headerFooter")) // CT_HeaderFooter 0-1
        {
            header_footer hf;

            hf.align_with_margins(
                !parser().attribute_present("alignWithMargins") || is_true(parser().attribute("alignWithMargins")));
            hf.scale_with_doc(
                !parser().attribute_present("alignWithMargins") || is_true(parser().attribute("alignWithMargins")));
            auto different_odd_even =
                parser().attribute_present("differentOddEven") && is_true(parser().attribute("differentOddEven"));
            auto different_first =
                parser().attribute_present("differentFirst") && is_true(parser().attribute("differentFirst"));

            optional<std::array<optional<rich_text>, 3>> odd_header;
            optional<std::array<optional<rich_text>, 3>> odd_footer;
            optional<std::array<optional<rich_text>, 3>> even_header;
            optional<std::array<optional<rich_text>, 3>> even_footer;
            optional<std::array<optional<rich_text>, 3>> first_header;
            optional<std::array<optional<rich_text>, 3>> first_footer;

            while (in_element(current_worksheet_element))
            {
                auto current_hf_element = expect_start_element(xml::content::simple);

                if (current_hf_element == qn("spreadsheetml", "oddHeader"))
                {
                    odd_header = parse_header_footer(read_text());
                }
                else if (current_hf_element == qn("spreadsheetml", "oddFooter"))
                {
                    odd_footer = parse_header_footer(read_text());
                }
                else if (current_hf_element == qn("spreadsheetml", "evenHeader"))
                {
                    even_header = parse_header_footer(read_text());
                }
                else if (current_hf_element == qn("spreadsheetml", "evenFooter"))
                {
                    even_footer = parse_header_footer(read_text());
                }
                else if (current_hf_element == qn("spreadsheetml", "firstHeader"))
                {
                    first_header = parse_header_footer(read_text());
                }
                else if (current_hf_element == qn("spreadsheetml", "firstFooter"))
                {
                    first_footer = parse_header_footer(read_text());
                }
                else
                {
                    unexpected_element(current_hf_element);
                }

                expect_end_element(current_hf_element);
            }

            for (std::size_t i = 0; i < 3; ++i)
            {
                auto loc = i == 0 ? header_footer::location::left
                                  : i == 1 ? header_footer::location::center : header_footer::location::right;

                if (different_odd_even)
                {
                    if (odd_header.is_set() && odd_header.get().at(i).is_set() && even_header.is_set()
                        && even_header.get().at(i).is_set())
                    {
                        hf.odd_even_header(loc, odd_header.get().at(i).get(), even_header.get().at(i).get());
                    }

                    if (odd_footer.is_set() && odd_footer.get().at(i).is_set() && even_footer.is_set()
                        && even_footer.get().at(i).is_set())
                    {
                        hf.odd_even_footer(loc, odd_footer.get().at(i).get(), even_footer.get().at(i).get());
                    }
                }
                else
                {
                    if (odd_header.is_set() && odd_header.get().at(i).is_set())
                    {
                        hf.header(loc, odd_header.get().at(i).get());
                    }

                    if (odd_footer.is_set() && odd_footer.get().at(i).is_set())
                    {
                        hf.footer(loc, odd_footer.get().at(i).get());
                    }
                }

                if (different_first)
                {
                }
            }

            ws.header_footer(hf);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "rowBreaks")) // CT_PageBreak 0-1
        {
            auto count = parser().attribute_present("count") ? parser().attribute<std::size_t>("count") : 0;
            auto manual_break_count = parser().attribute_present("manualBreakCount")
                ? parser().attribute<std::size_t>("manualBreakCount") : 0;

            while (in_element(qn("spreadsheetml", "rowBreaks")))
            {
                expect_start_element(qn("spreadsheetml", "brk"), xml::content::simple);

                if (parser().attribute_present("id"))
                {
                    ws.page_break_at_row(parser().attribute<row_t>("id"));
                    --count;
                }

                if (parser().attribute_present("man") && is_true(parser().attribute("man")))
                {
                    --manual_break_count;
                }

                skip_attributes({"min", "max", "pt"});
                expect_end_element(qn("spreadsheetml", "brk"));
            }
        }
        else if (current_worksheet_element == qn("spreadsheetml", "colBreaks")) // CT_PageBreak 0-1
        {
            auto count = parser().attribute_present("count") ? parser().attribute<std::size_t>("count") : 0;
            auto manual_break_count = parser().attribute_present("manualBreakCount")
                ? parser().attribute<std::size_t>("manualBreakCount")
                : 0;

            while (in_element(qn("spreadsheetml", "colBreaks")))
            {
                expect_start_element(qn("spreadsheetml", "brk"), xml::content::simple);

                if (parser().attribute_present("id"))
                {
                    ws.page_break_at_column(parser().attribute<column_t::index_t>("id"));
                    --count;
                }

                if (parser().attribute_present("man") && is_true(parser().attribute("man")))
                {
                    --manual_break_count;
                }

                skip_attributes({"min", "max", "pt"});
                expect_end_element(qn("spreadsheetml", "brk"));
            }
        }
        else if (current_worksheet_element == qn("spreadsheetml", "customProperties")) // CT_CustomProperties 0-1
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "cellWatches")) // CT_CellWatches 0-1
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "ignoredErrors")) // CT_IgnoredErrors 0-1
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "smartTags")) // CT_SmartTags 0-1
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "drawing")) // CT_Drawing 0-1
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "legacyDrawing"))
        {
            skip_remaining_content(current_worksheet_element);
        }
        else if (current_worksheet_element == qn("spreadsheetml", "extLst"))
        {
            skip_remaining_content(current_worksheet_element);
        }
        else
        {
            unexpected_element(current_worksheet_element);
        }

        expect_end_element(current_worksheet_element);
    }

    expect_end_element(qn("spreadsheetml", "worksheet"));

    if (manifest.has_relationship(sheet_path, xlnt::relationship_type::comments))
    {
        auto comments_part = manifest.canonicalize(
            {workbook_rel, sheet_rel, manifest.relationship(sheet_path, xlnt::relationship_type::comments)});

        auto receive = xml::parser::receive_default;
        auto comments_part_streambuf = archive_->open(comments_part);
        std::istream comments_part_stream(comments_part_streambuf.get());
        xml::parser parser(comments_part_stream, comments_part.string(), receive);
        parser_ = &parser;

        read_comments(ws);

        if (manifest.has_relationship(sheet_path, xlnt::relationship_type::vml_drawing))
        {
            auto vml_drawings_part = manifest.canonicalize(
                {workbook_rel, sheet_rel, manifest.relationship(sheet_path, xlnt::relationship_type::vml_drawing)});

            auto vml_drawings_part_streambuf = archive_->open(comments_part);
            std::istream vml_drawings_part_stream(comments_part_streambuf.get());
            xml::parser vml_parser(vml_drawings_part_stream, vml_drawings_part.string(), receive);
            parser_ = &vml_parser;

            read_vml_drawings(ws);
        }
    }
}

// Sheet Relationship Target Parts

void xlsx_consumer::read_vml_drawings(worksheet /*ws*/)
{
}

void xlsx_consumer::read_comments(worksheet ws)
{
    std::vector<std::string> authors;

    expect_start_element(qn("spreadsheetml", "comments"), xml::content::complex);
    // name space can be ignored
    skip_attribute(qn("mc","Ignorable"));
    expect_start_element(qn("spreadsheetml", "authors"), xml::content::complex);

    while (in_element(qn("spreadsheetml", "authors")))
    {
        expect_start_element(qn("spreadsheetml", "author"), xml::content::simple);
        authors.push_back(read_text());
        expect_end_element(qn("spreadsheetml", "author"));
    }

    expect_end_element(qn("spreadsheetml", "authors"));
    expect_start_element(qn("spreadsheetml", "commentList"), xml::content::complex);

    while (in_element(xml::qname(qn("spreadsheetml", "commentList"))))
    {
        expect_start_element(qn("spreadsheetml", "comment"), xml::content::complex);

        skip_attribute("shapeId");
        auto cell_ref = parser().attribute("ref");
        auto author_id = parser().attribute<std::size_t>("authorId");

        expect_start_element(qn("spreadsheetml", "text"), xml::content::complex);

        ws.cell(cell_ref).comment(comment(read_rich_text(qn("spreadsheetml", "text")), authors.at(author_id)));

        expect_end_element(qn("spreadsheetml", "text"));

	if (in_element(xml::qname(qn("spreadsheetml", "comment"))))
	{
	    expect_start_element(qn("mc", "AlternateContent"), xml::content::complex);
	    skip_remaining_content(qn("mc", "AlternateContent"));
	    expect_end_element(qn("mc", "AlternateContent"));
	}

        expect_end_element(qn("spreadsheetml", "comment"));
    }

    expect_end_element(qn("spreadsheetml", "commentList"));
    expect_end_element(qn("spreadsheetml", "comments"));
}

void xlsx_consumer::read_drawings()
{
}

// Unknown Parts

void xlsx_consumer::read_unknown_parts()
{
}

void xlsx_consumer::read_unknown_relationships()
{
}

void xlsx_consumer::read_image(const xlnt::path &image_path)
{
    auto image_streambuf = archive_->open(image_path);
    vector_ostreambuf buffer(target_.d_->images_[image_path.string()]);
    std::ostream out_stream(&buffer);
    out_stream << image_streambuf.get();
}

std::string xlsx_consumer::read_text()
{
    auto text = std::string();

    while (parser().peek() == xml::parser::event_type::characters)
    {
        parser().next_expect(xml::parser::event_type::characters);
        text.append(parser().value());
    }

    return text;
}

variant xlsx_consumer::read_variant()
{
    auto value = variant(read_text());

    if (in_element(stack_.back()))
    {
        auto element = expect_start_element(xml::content::mixed);
        auto text = read_text();

        if (element == qn("vt", "lpwstr") || element == qn("vt", "lpstr"))
        {
            value = variant(text);
        }
        if (element == qn("vt", "i4"))
        {
            value = variant(std::stoi(text));
        }
        if (element == qn("vt", "bool"))
        {
            value = variant(is_true(text));
        }
        else if (element == qn("vt", "vector"))
        {
            auto size = parser().attribute<std::size_t>("size");
            auto base_type = parser().attribute("baseType");

            std::vector<variant> vector;

            for (auto i = std::size_t(0); i < size; ++i)
            {
                if (base_type == "variant")
                {
                    expect_start_element(qn("vt", "variant"), xml::content::complex);
                }

                vector.push_back(read_variant());

                if (base_type == "variant")
                {
                    expect_end_element(qn("vt", "variant"));
                    read_text();
                }
            }

            value = variant(vector);
        }

        expect_end_element(element);
        read_text();
    }

    return value;
}

void xlsx_consumer::skip_attributes(const std::vector<std::string> &names)
{
    for (const auto &name : names)
    {
        if (parser().attribute_present(name))
        {
            parser().attribute(name);
        }
    }
}

void xlsx_consumer::skip_attributes(const std::vector<xml::qname> &names)
{
    for (const auto &name : names)
    {
        if (parser().attribute_present(name))
        {
            parser().attribute(name);
        }
    }
}

void xlsx_consumer::skip_attributes()
{
    parser().attribute_map();
}

void xlsx_consumer::skip_attribute(const xml::qname &name)
{
    if (parser().attribute_present(name))
    {
        parser().attribute(name);
    }
}

void xlsx_consumer::skip_attribute(const std::string &name)
{
    if (parser().attribute_present(name))
    {
        parser().attribute(name);
    }
}

void xlsx_consumer::skip_remaining_content(const xml::qname &name)
{
    // start by assuming we've already parsed the opening tag

    skip_attributes();
    read_namespaces();
    read_text();

    // continue until the closing tag is reached
    while (in_element(name))
    {
        auto child_element = expect_start_element(xml::content::mixed);
        skip_remaining_content(child_element);
        expect_end_element(child_element);
        read_text(); // trailing character content (usually whitespace)
    }
}

std::vector<std::string> xlsx_consumer::read_namespaces()
{
    std::vector<std::string> namespaces;

    while (parser().peek() == xml::parser::event_type::start_namespace_decl)
    {
        parser().next_expect(xml::parser::event_type::start_namespace_decl);
        namespaces.push_back(parser().namespace_());

        if (parser().peek() == xml::parser::event_type::end_namespace_decl)
        {
            parser().next_expect(xml::parser::event_type::end_namespace_decl);
        }
    }

    return namespaces;
}

bool xlsx_consumer::in_element(const xml::qname &name)
{
    return parser().peek() != xml::parser::event_type::end_element
        && stack_.back() == name;
}

xml::qname xlsx_consumer::expect_start_element(xml::content content)
{
    parser().next_expect(xml::parser::event_type::start_element);
    parser().content(content);
    stack_.push_back(parser().qname());

    const auto xml_space = qn("xml", "space");
    preserve_space_ = parser().attribute_present(xml_space) ? parser().attribute(xml_space) == "preserve" : false;

    return stack_.back();
}

void xlsx_consumer::expect_start_element(const xml::qname &name, xml::content content)
{
    parser().next_expect(xml::parser::event_type::start_element, name);
    parser().content(content);
    stack_.push_back(name);

    const auto xml_space = qn("xml", "space");
    preserve_space_ = parser().attribute_present(xml_space) ? parser().attribute(xml_space) == "preserve" : false;
}

void xlsx_consumer::expect_end_element(const xml::qname &name)
{
    parser().next_expect(xml::parser::event_type::end_element, name);

    while (parser().peek() == xml::parser::event_type::end_namespace_decl)
    {
        parser().next_expect(xml::parser::event_type::end_namespace_decl);
    }

    stack_.pop_back();
}

rich_text xlsx_consumer::read_rich_text(const xml::qname &parent)
{
    const auto &xmlns = parent.namespace_();
    rich_text t;

    while (in_element(parent))
    {
        auto text_element = expect_start_element(xml::content::mixed);
        skip_attributes();
        auto text = read_text();

        if (text_element == xml::qname(xmlns, "t"))
        {
            t.plain_text(text);
        }
        else if (text_element == xml::qname(xmlns, "r"))
        {
            rich_text_run run;

            while (in_element(xml::qname(xmlns, "r")))
            {
                auto run_element = expect_start_element(xml::content::mixed);
                auto run_text = read_text();

                if (run_element == xml::qname(xmlns, "rPr"))
                {
                    while (in_element(xml::qname(xmlns, "rPr")))
                    {
                        auto current_run_property_element = expect_start_element(xml::content::simple);

                        run.second = xlnt::font();

                        if (current_run_property_element == xml::qname(xmlns, "sz"))
                        {
                            run.second.get().size(parser().attribute<double>("val"));
                        }
                        else if (current_run_property_element == xml::qname(xmlns, "rFont"))
                        {
                            run.second.get().name(parser().attribute("val"));
                        }
                        else if (current_run_property_element == xml::qname(xmlns, "color"))
                        {
                            run.second.get().color(read_color());
                        }
                        else if (current_run_property_element == xml::qname(xmlns, "family"))
                        {
                            run.second.get().family(parser().attribute<std::size_t>("val"));
                        }
                        else if (current_run_property_element == xml::qname(xmlns, "charset"))
                        {
                            run.second.get().charset(parser().attribute<std::size_t>("val"));
                        }
                        else if (current_run_property_element == xml::qname(xmlns, "scheme"))
                        {
                            run.second.get().scheme(parser().attribute("val"));
                        }
                        else if (current_run_property_element == xml::qname(xmlns, "b"))
                        {
                            run.second.get().bold(parser().attribute_present("val")
                                ? is_true(parser().attribute("val")) : true);
                        }
                        else if (current_run_property_element == xml::qname(xmlns, "i"))
                        {
                            run.second.get().italic(parser().attribute_present("val")
                                ? is_true(parser().attribute("val")) : true);
                        }
                        else if (current_run_property_element == xml::qname(xmlns, "u"))
                        {
                            if (parser().attribute_present("val"))
                            {
                                run.second.get().underline(parser().attribute<font::underline_style>("val"));
                            }
                            else
                            {
                                run.second.get().underline(font::underline_style::single);
                            }
                        }
                        else
                        {
                            unexpected_element(current_run_property_element);
                        }

                        expect_end_element(current_run_property_element);
                        read_text();
                    }
                }
                else if (run_element == xml::qname(xmlns, "t"))
                {
                    run.first = run_text;
                }
                else
                {
                    unexpected_element(run_element);
                }

                read_text();
                expect_end_element(run_element);
                read_text();
            }

            t.add_run(run);
        }
        else if (text_element == xml::qname(xmlns, "rPh"))
        {
            skip_remaining_content(text_element);
        }
        else if (text_element == xml::qname(xmlns, "phoneticPr"))
        {
            skip_remaining_content(text_element);
        }
        else
        {
            unexpected_element(text_element);
        }

        read_text();
        expect_end_element(text_element);
    }

    return t;
}

xlnt::color xlsx_consumer::read_color()
{
    xlnt::color result;

    if (parser().attribute_present("auto") && is_true(parser().attribute("auto")))
    {
        result.auto_(true);
        return result;
    }

    if (parser().attribute_present("rgb"))
    {
        result = xlnt::rgb_color(parser().attribute("rgb"));
    }
    else if (parser().attribute_present("theme"))
    {
        result = xlnt::theme_color(parser().attribute<std::size_t>("theme"));
    }
    else if (parser().attribute_present("indexed"))
    {
        result = xlnt::indexed_color(parser().attribute<std::size_t>("indexed"));
    }

    if (parser().attribute_present("tint"))
    {
        result.tint(parser().attribute("tint", 0.0));
    }

    return result;
}

manifest &xlsx_consumer::manifest()
{
    return target_.manifest();
}

} // namespace detail
} // namepsace xlnt
