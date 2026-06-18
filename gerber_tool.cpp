/*
 * gerber_tool.cpp — Outil de conversion Gerber <-> SVG
 *
 * Compile: g++ -std=c++17 -O2 -o gerber_tool gerber_tool.cpp
 *          clang++ -std=c++17 -O2 -o gerber_tool gerber_tool.cpp
 *
 * Usage:
 *   ./gerber_tool --svg  board.GTO [output.svg] [--outline board.GKO]
 *   ./gerber_tool --gerber board.GTO.svg [output.GTO]
 *
 * Zero dependencies, single file, C++17.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

static constexpr double INCH_TO_MM = 25.4;
static constexpr double MM_TO_INCH = 1.0 / 25.4;
static constexpr double PI = 3.14159265358979323846;

// =========================================================================
//  Data structures
// =========================================================================

struct FormatSpec {
    char zero_omit  = 'L';
    char coord_mode = 'A';
    int x_int = 2, x_dec = 5;
    int y_int = 2, y_dec = 5;
    bool valid = false;
};

struct Aperture {
    std::string type;
    std::vector<double> params;
    std::vector<std::string> params_raw;
};

struct Operation {
    std::string type; // "line" or "flash"
    int aperture = 0;
    double x1=0, y1=0, x2=0, y2=0; // line
    double x=0, y=0;               // flash
};

struct GerberInfo {
    std::string source_file;
    std::string units = "inch";
    FormatSpec format;
    std::map<int, Aperture> apertures;
    std::map<std::string, std::vector<std::string>> macros;
    std::vector<Operation> operations;
    int edit_aperture = -1;
    char polarity = 'D';
};

using Point    = std::pair<double, double>;
using Polyline = std::vector<Point>;

struct SvgElements {
    std::map<int, std::vector<Polyline>> traces;
    std::map<int, std::vector<Point>>    flashes;
    std::map<int, std::vector<Polyline>> regions;       // dark fills
    std::map<int, std::vector<Polyline>> regions_clear;  // holes
};

// =========================================================================
//  Utility
// =========================================================================

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::istringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, delim)) parts.push_back(tok);
    return parts;
}

static std::string basename(const std::string& path) {
    auto p = path.find_last_of("/\\");
    return (p == std::string::npos) ? path : path.substr(p + 1);
}

static std::string stem(const std::string& filename) {
    auto p = filename.rfind('.');
    return (p == std::string::npos) ? filename : filename.substr(0, p);
}

static bool ends_with(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) { std::cerr << "Cannot open: " << path << "\n"; exit(1); }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (!f) { std::cerr << "Cannot write: " << path << "\n"; exit(1); }
    f << content;
}

static double to_mm(double val, const std::string& units) {
    return units == "inch" ? val * INCH_TO_MM : val;
}

static char fmt_buf[64];
static std::string fmt4(double v) {
    snprintf(fmt_buf, sizeof(fmt_buf), "%.4f", v);
    return fmt_buf;
}

// =========================================================================
//  GERBER PARSING
// =========================================================================

static FormatSpec parse_format_spec(const std::string& line) {
    FormatSpec fs;
    // %FSLAX25Y25*%
    std::regex re(R"(%FS([LA])([AI])X(\d)(\d)Y(\d)(\d)\*%)");
    std::smatch m;
    if (std::regex_match(line, m, re)) {
        fs.zero_omit  = m[1].str()[0];
        fs.coord_mode = m[2].str()[0];
        fs.x_int = std::stoi(m[3]); fs.x_dec = std::stoi(m[4]);
        fs.y_int = std::stoi(m[5]); fs.y_dec = std::stoi(m[6]);
        fs.valid = true;
    }
    return fs;
}

static bool parse_aperture_def(const std::string& line, int& id, Aperture& ap) {
    std::regex re(R"(%ADD(\d+)([A-Za-z][A-Za-z0-9]*),?(.*?)\*%)");
    std::smatch m;
    if (!std::regex_match(line, m, re)) return false;
    id = std::stoi(m[1]);
    ap.type = m[2];
    std::string ps = m[3];
    if (!ps.empty()) {
        ap.params_raw = split(ps, 'X');
        for (auto& r : ap.params_raw) ap.params.push_back(std::stod(r));
    }
    return true;
}

static double parse_coord(const std::string& s, int n_dec) {
    return std::stod(s) / std::pow(10.0, n_dec);
}

static GerberInfo parse_gerber(const std::string& filepath) {
    std::string raw = read_file(filepath);
    // Split into lines
    std::vector<std::string> lines;
    std::istringstream ss(raw);
    std::string ln;
    while (std::getline(ss, ln)) {
        ln = trim(ln);
        if (!ln.empty()) lines.push_back(ln);
    }

    GerberInfo info;
    info.source_file = basename(filepath);

    // Parse macros
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].substr(0, 3) == "%AM") {
            std::string name = lines[i].substr(3);
            if (!name.empty() && name.back() == '*') name.pop_back();
            std::vector<std::string> body;
            i++;
            while (i < lines.size() && lines[i][0] != '%') {
                std::string bl = lines[i];
                if (!bl.empty() && bl.back() == '*') bl.pop_back();
                body.push_back(bl);
                i++;
            }
            info.macros[name] = body;
        }
    }

    int cur_ap = 0;
    double cx = 0, cy = 0;

    std::regex coord_re(R"(^(?:G\d+)?(?:X([+-]?\d+))?(?:Y([+-]?\d+))?D(\d+)\*$)");

    for (auto& line : lines) {
        if (line == "%MOIN*%") { info.units = "inch"; continue; }
        if (line == "%MOMM*%") { info.units = "mm"; continue; }
        if (line.substr(0, 3) == "%FS") {
            info.format = parse_format_spec(line);
            continue;
        }
        if (line.substr(0, 4) == "%ADD") {
            int id; Aperture ap;
            if (parse_aperture_def(line, id, ap))
                info.apertures[id] = ap;
            continue;
        }
        // Polarity
        if (line.size() >= 6 && line.substr(0, 3) == "%LP") {
            info.polarity = line[3];
            continue;
        }
        if (line[0] == '%') continue;
        // G-codes, M-codes
        std::regex gm_re(R"(^[GM]\d+\*$)");
        if (std::regex_match(line, gm_re)) continue;
        // Aperture select
        std::regex dsel_re(R"(^D(\d+)\*$)");
        std::smatch dm;
        if (std::regex_match(line, dm, dsel_re)) {
            int d = std::stoi(dm[1]);
            if (d >= 10) cur_ap = d;
            continue;
        }
        if (!info.format.valid) continue;
        // Coord commands
        std::smatch cm;
        if (std::regex_match(line, cm, coord_re)) {
            double nx = cm[1].matched ? parse_coord(cm[1], info.format.x_dec) : cx;
            double ny = cm[2].matched ? parse_coord(cm[2], info.format.y_dec) : cy;
            int dc = std::stoi(cm[3]);
            if (dc == 2) {
                cx = nx; cy = ny;
            } else if (dc == 1) {
                Operation op;
                op.type = "line"; op.aperture = cur_ap;
                op.x1 = cx; op.y1 = cy; op.x2 = nx; op.y2 = ny;
                info.operations.push_back(op);
                cx = nx; cy = ny;
            } else if (dc == 3) {
                Operation op;
                op.type = "flash"; op.aperture = cur_ap;
                op.x = nx; op.y = ny;
                info.operations.push_back(op);
                cx = nx; cy = ny;
            }
        }
    }
    return info;
}

// =========================================================================
//  JSON WRITE / READ
// =========================================================================

static std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

static std::string write_json(const GerberInfo& info) {
    std::ostringstream j;
    j << std::setprecision(10);
    j << "{\n";
    j << "  \"source_file\": \"" << json_escape(info.source_file) << "\",\n";
    j << "  \"units\": \"" << info.units << "\",\n";
    j << "  \"polarity\": \"" << info.polarity << "\",\n";

    // Format
    j << "  \"format\": {\n";
    j << "    \"zero_omit\": \"" << info.format.zero_omit << "\",\n";
    j << "    \"coord_mode\": \"" << info.format.coord_mode << "\",\n";
    j << "    \"x_int\": " << info.format.x_int << ", \"x_dec\": " << info.format.x_dec << ",\n";
    j << "    \"y_int\": " << info.format.y_int << ", \"y_dec\": " << info.format.y_dec << "\n";
    j << "  },\n";

    // Apertures
    j << "  \"apertures\": {\n";
    int ai = 0;
    for (auto& [id, ap] : info.apertures) {
        j << "    \"" << id << "\": { \"type\": \"" << ap.type << "\", \"params\": [";
        for (size_t i = 0; i < ap.params.size(); i++) {
            if (i) j << ", ";
            j << ap.params[i];
        }
        j << "], \"params_raw\": [";
        for (size_t i = 0; i < ap.params_raw.size(); i++) {
            if (i) j << ", ";
            j << "\"" << ap.params_raw[i] << "\"";
        }
        j << "] }";
        if (++ai < (int)info.apertures.size()) j << ",";
        j << "\n";
    }
    j << "  },\n";

    // Macros
    j << "  \"macros\": {\n";
    int mi = 0;
    for (auto& [name, body] : info.macros) {
        j << "    \"" << name << "\": [";
        for (size_t i = 0; i < body.size(); i++) {
            if (i) j << ", ";
            j << "\"" << json_escape(body[i]) << "\"";
        }
        j << "]";
        if (++mi < (int)info.macros.size()) j << ",";
        j << "\n";
    }
    j << "  },\n";

    // edit_aperture
    j << "  \"edit_aperture\": " << info.edit_aperture << ",\n";

    // Operations
    j << "  \"operations\": [\n";
    for (size_t i = 0; i < info.operations.size(); i++) {
        auto& op = info.operations[i];
        j << "    { \"type\": \"" << op.type << "\", \"aperture\": " << op.aperture;
        if (op.type == "line")
            j << ", \"x1\": " << op.x1 << ", \"y1\": " << op.y1
              << ", \"x2\": " << op.x2 << ", \"y2\": " << op.y2;
        else
            j << ", \"x\": " << op.x << ", \"y\": " << op.y;
        j << " }";
        if (i + 1 < info.operations.size()) j << ",";
        j << "\n";
    }
    j << "  ]\n";
    j << "}\n";
    return j.str();
}

// Minimal targeted JSON reader for our format
struct JsonValue {
    enum Type { NONE, STRING, NUMBER, OBJECT, ARRAY } type = NONE;
    std::string sval;
    double nval = 0;
    std::map<std::string, JsonValue> obj;
    std::vector<JsonValue> arr;
};

static size_t skip_ws(const std::string& s, size_t i) {
    while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) i++;
    return i;
}

static JsonValue parse_json_value(const std::string& s, size_t& i);

static std::string parse_json_string(const std::string& s, size_t& i) {
    i++; // skip "
    std::string out;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i+1 < s.size()) { i++; out += s[i]; }
        else out += s[i];
        i++;
    }
    if (i < s.size()) i++; // skip "
    return out;
}

static JsonValue parse_json_value(const std::string& s, size_t& i) {
    i = skip_ws(s, i);
    JsonValue v;
    if (i >= s.size()) return v;

    if (s[i] == '"') {
        v.type = JsonValue::STRING;
        v.sval = parse_json_string(s, i);
    } else if (s[i] == '{') {
        v.type = JsonValue::OBJECT;
        i++; i = skip_ws(s, i);
        while (i < s.size() && s[i] != '}') {
            i = skip_ws(s, i);
            std::string key = parse_json_string(s, i);
            i = skip_ws(s, i);
            if (i < s.size() && s[i] == ':') i++;
            v.obj[key] = parse_json_value(s, i);
            i = skip_ws(s, i);
            if (i < s.size() && s[i] == ',') i++;
        }
        if (i < s.size()) i++;
    } else if (s[i] == '[') {
        v.type = JsonValue::ARRAY;
        i++; i = skip_ws(s, i);
        while (i < s.size() && s[i] != ']') {
            v.arr.push_back(parse_json_value(s, i));
            i = skip_ws(s, i);
            if (i < s.size() && s[i] == ',') i++;
        }
        if (i < s.size()) i++;
    } else {
        // number (int, float, negative)
        v.type = JsonValue::NUMBER;
        size_t start = i;
        if (s[i] == '-') i++;
        while (i < s.size() && (isdigit(s[i]) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) {
            if ((s[i] == '+' || s[i] == '-') && i > start && s[i-1] != 'e' && s[i-1] != 'E') break;
            i++;
        }
        v.nval = std::stod(s.substr(start, i - start));
        v.sval = s.substr(start, i - start);
    }
    return v;
}

static GerberInfo read_json_info(const std::string& path) {
    std::string raw = read_file(path);
    size_t pos = 0;
    JsonValue root = parse_json_value(raw, pos);

    GerberInfo info;
    info.source_file = root.obj["source_file"].sval;
    info.units = root.obj["units"].sval;

    auto& fmt = root.obj["format"].obj;
    info.format.zero_omit  = fmt["zero_omit"].sval[0];
    info.format.coord_mode = fmt["coord_mode"].sval[0];
    info.format.x_int = (int)fmt["x_int"].nval;
    info.format.x_dec = (int)fmt["x_dec"].nval;
    info.format.y_int = (int)fmt["y_int"].nval;
    info.format.y_dec = (int)fmt["y_dec"].nval;
    info.format.valid = true;

    for (auto& [key, val] : root.obj["apertures"].obj) {
        int id = std::stoi(key);
        Aperture ap;
        ap.type = val.obj["type"].sval;
        for (auto& p : val.obj["params"].arr) ap.params.push_back(p.nval);
        for (auto& p : val.obj["params_raw"].arr) ap.params_raw.push_back(p.sval);
        info.apertures[id] = ap;
    }

    for (auto& [key, val] : root.obj["macros"].obj) {
        std::vector<std::string> body;
        for (auto& b : val.arr) body.push_back(b.sval);
        info.macros[key] = body;
    }

    if (root.obj.count("edit_aperture"))
        info.edit_aperture = (int)root.obj["edit_aperture"].nval;

    return info;
}

// =========================================================================
//  SVG GENERATION (Gerber -> SVG)
// =========================================================================

static double aperture_diameter_mm(const Aperture& ap,
        const std::map<std::string,std::vector<std::string>>& macros,
        const std::string& units) {
    if (ap.type == "C")
        return ap.params.empty() ? 0 : to_mm(ap.params[0], units);
    if (ap.type == "R") {
        double w = ap.params.size() > 0 ? to_mm(ap.params[0], units) : 0;
        double h = ap.params.size() > 1 ? to_mm(ap.params[1], units) : w;
        return std::max(w, h);
    }
    if (macros.count(ap.type)) {
        for (auto& body : macros.at(ap.type)) {
            auto parts = split(body, ',');
            if (!parts.empty() && trim(parts[0]) == "5" && parts.size() > 5) {
                std::string diam_expr = trim(parts[5]);
                double diam;
                if (diam_expr.find('X') != std::string::npos && diam_expr.find('$') != std::string::npos) {
                    double factor = std::stod(diam_expr.substr(0, diam_expr.find('X')));
                    diam = factor * (ap.params.empty() ? 0 : ap.params[0]);
                } else {
                    diam = std::stod(diam_expr);
                }
                return to_mm(diam, units);
            }
        }
    }
    return ap.params.empty() ? 0 : to_mm(ap.params[0], units);
}

static std::string make_flash_svg(const Aperture& ap,
        const std::map<std::string,std::vector<std::string>>& macros,
        double xm, double ym, const std::string& units, int ap_id) {
    std::ostringstream s;
    if (ap.type == "C") {
        double r = ap.params.empty() ? 0 : to_mm(ap.params[0], units) / 2;
        if (r < 1e-9)
            s << "<circle cx=\""<<fmt4(xm)<<"\" cy=\""<<fmt4(ym)<<"\" r=\"0.05\" class=\"ap"<<ap_id<<" flash zero-size\"";
        else
            s << "<circle cx=\""<<fmt4(xm)<<"\" cy=\""<<fmt4(ym)<<"\" r=\""<<fmt4(r)<<"\" class=\"ap"<<ap_id<<" flash\"";
        return s.str() + "/>";
    }
    if (ap.type == "R") {
        double w = ap.params.size() > 0 ? to_mm(ap.params[0], units) : 0;
        double h = ap.params.size() > 1 ? to_mm(ap.params[1], units) : w;
        s << "<rect x=\""<<fmt4(xm-w/2)<<"\" y=\""<<fmt4(ym-h/2)
          <<"\" width=\""<<fmt4(w)<<"\" height=\""<<fmt4(h)
          <<"\" class=\"ap"<<ap_id<<" flash\"/>";
        return s.str();
    }
    // Macro polygon
    if (macros.count(ap.type)) {
        for (auto& body : macros.at(ap.type)) {
            auto parts = split(body, ',');
            if (!parts.empty() && trim(parts[0]) == "5" && parts.size() > 6) {
                int nv = std::stoi(trim(parts[2]));
                std::string de = trim(parts[5]);
                double rot = std::stod(trim(parts[6]));
                double diam;
                if (de.find('X') != std::string::npos) {
                    double fac = std::stod(de.substr(0, de.find('X')));
                    diam = fac * ap.params[0];
                } else diam = std::stod(de);
                double r = to_mm(diam, units) / 2;
                s << "<polygon points=\"";
                for (int i = 0; i < nv; i++) {
                    double a = (rot + i * 360.0 / nv) * PI / 180.0;
                    if (i) s << " ";
                    s << fmt4(xm + r*cos(a)) << "," << fmt4(ym + r*sin(a));
                }
                s << "\" class=\"ap"<<ap_id<<" flash\"/>";
                return s.str();
            }
        }
    }
    s << "<circle cx=\""<<fmt4(xm)<<"\" cy=\""<<fmt4(ym)<<"\" r=\"0.1\" class=\"ap"<<ap_id<<" flash\"/>";
    return s.str();
}

static std::vector<Polyline> build_polylines(const std::vector<Operation>& ops, const std::string& units) {
    std::vector<Polyline> paths;
    Polyline cur;
    for (auto& op : ops) {
        if (op.type != "line") continue;
        double x1 = to_mm(op.x1, units), y1 = -to_mm(op.y1, units);
        double x2 = to_mm(op.x2, units), y2 = -to_mm(op.y2, units);
        if (!cur.empty() && std::abs(cur.back().first-x1)<1e-4 && std::abs(cur.back().second-y1)<1e-4) {
            cur.push_back({x2, y2});
        } else {
            if (!cur.empty()) paths.push_back(cur);
            cur = {{x1,y1},{x2,y2}};
        }
    }
    if (!cur.empty()) paths.push_back(cur);
    return paths;
}

static std::string generate_svg(const GerberInfo& info,
        const std::vector<Operation>* outline_ops = nullptr,
        const GerberInfo* outline_info = nullptr) {
    auto& units = info.units;
    double all_min_x=1e9, all_max_x=-1e9, all_min_y=1e9, all_max_y=-1e9;

    auto update_bounds = [&](double x, double y) {
        all_min_x = std::min(all_min_x, x); all_max_x = std::max(all_max_x, x);
        all_min_y = std::min(all_min_y, y); all_max_y = std::max(all_max_y, y);
    };

    for (auto& op : info.operations) {
        if (op.type == "line") { update_bounds(op.x1,op.y1); update_bounds(op.x2,op.y2); }
        else { update_bounds(op.x, op.y); }
    }
    if (outline_ops)
        for (auto& op : *outline_ops)
            if (op.type == "line") { update_bounds(op.x1,op.y1); update_bounds(op.x2,op.y2); }

    double margin = 0.05;
    all_min_x -= margin; all_max_x += margin;
    all_min_y -= margin; all_max_y += margin;

    double vb_x = to_mm(all_min_x, units);
    double vb_w = to_mm(all_max_x - all_min_x, units);
    double vb_y = -to_mm(all_max_y, units);
    double vb_h = to_mm(all_max_y - all_min_y, units);

    std::ostringstream s;
    s << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<svg xmlns=\"http://www.w3.org/2000/svg\"\n"
      << "     xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\"\n"
      << "     xmlns:sodipodi=\"http://sodipodi.sourceforge.net/DTD/sodipodi-0.0.dtd\"\n"
      << "     width=\""<<fmt4(vb_w)<<"mm\" height=\""<<fmt4(vb_h)<<"mm\"\n"
      << "     viewBox=\""<<fmt4(vb_x)<<" "<<fmt4(vb_y)<<" "<<fmt4(vb_w)<<" "<<fmt4(vb_h)<<"\">\n\n"
      << "  <defs><style>\n"
      << "    .trace { fill: none; stroke: #b02020; stroke-linecap: round; stroke-linejoin: round; }\n"
      << "    .flash { fill: #b02020; stroke: none; }\n"
      << "    .zero-size { fill: #ff00ff; opacity: 0.3; }\n"
      << "  </style></defs>\n\n"
      << "  <rect x=\""<<fmt4(vb_x)<<"\" y=\""<<fmt4(vb_y)<<"\" width=\""<<fmt4(vb_w)
      << "\" height=\""<<fmt4(vb_h)<<"\" fill=\"#1a1a2e\" id=\"background\"/>\n\n";

    // Outline layer
    if (outline_ops && outline_info) {
        s << "  <g id=\"outline-ref\" inkscape:groupmode=\"layer\""
          << " inkscape:label=\"[REF] Contour PCB\""
          << " sodipodi:insensitive=\"true\" style=\"opacity:0.6\">\n";
        auto olines = build_polylines(*outline_ops, outline_info->units);
        for (size_t i = 0; i < olines.size(); i++) {
            s << "    <path d=\"M"<<fmt4(olines[i][0].first)<<","<<fmt4(olines[i][0].second);
            for (size_t j=1; j<olines[i].size(); j++)
                s << " L"<<fmt4(olines[i][j].first)<<","<<fmt4(olines[i][j].second);
            s << "\" stroke=\"#f0c030\" stroke-width=\"0.15\" fill=\"none\""
              << " stroke-dasharray=\"1,0.5\" id=\"outline-path-"<<i<<"\"/>\n";
        }
        s << "  </g>\n\n";
    }

    // Group ops by aperture
    std::map<int, std::vector<const Operation*>> by_ap;
    for (auto& op : info.operations) by_ap[op.aperture].push_back(&op);

    for (auto& [ap_id, ops] : by_ap) {
        auto it = info.apertures.find(ap_id);
        if (it == info.apertures.end()) continue;
        auto& ap = it->second;
        double diam = aperture_diameter_mm(ap, info.macros, units);
        std::string desc = ap.type + "(";
        for (size_t i=0; i<ap.params.size(); i++) { if (i) desc+=","; desc+=std::to_string(ap.params[i]); }
        desc += ")";

        s << "  <g id=\"aperture-D"<<ap_id<<"\" inkscape:groupmode=\"layer\""
          << " inkscape:label=\"D"<<ap_id<<" "<<desc<<"\">\n";

        // Traces
        std::vector<Operation> trace_ops;
        for (auto* o : ops) if (o->type == "line") trace_ops.push_back(*o);
        if (!trace_ops.empty()) {
            s << "    <g id=\"traces-D"<<ap_id<<"\" class=\"traces\">\n";
            auto paths = build_polylines(trace_ops, units);
            for (size_t i=0; i<paths.size(); i++) {
                s << "      <path d=\"M"<<fmt4(paths[i][0].first)<<","<<fmt4(paths[i][0].second);
                for (size_t j=1; j<paths[i].size(); j++)
                    s << " L"<<fmt4(paths[i][j].first)<<","<<fmt4(paths[i][j].second);
                s << "\" stroke-width=\""<<fmt4(diam)<<"\" class=\"trace ap"<<ap_id
                  <<"\" id=\"trace-D"<<ap_id<<"-"<<i<<"\"/>\n";
            }
            s << "    </g>\n";
        }

        // Flashes
        std::vector<const Operation*> flash_ops;
        for (auto* o : ops) if (o->type == "flash") flash_ops.push_back(o);
        if (!flash_ops.empty()) {
            s << "    <g id=\"flashes-D"<<ap_id<<"\" class=\"flashes\">\n";
            for (size_t i=0; i<flash_ops.size(); i++) {
                double xm = to_mm(flash_ops[i]->x, units);
                double ym = -to_mm(flash_ops[i]->y, units);
                std::string el = make_flash_svg(ap, info.macros, xm, ym, units, ap_id);
                // Insert id
                auto pos = el.find("/>");
                if (pos != std::string::npos)
                    el.insert(pos, " id=\"flash-D" + std::to_string(ap_id) + "-" + std::to_string(i) + "\"");
                s << "      " << el << "\n";
            }
            s << "    </g>\n";
        }
        s << "  </g>\n\n";
    }

    // EDIT layer
    int edit_ap = -1;
    double edit_diam = 1e9;
    for (auto& [id, ap] : info.apertures) {
        if (ap.type == "C" && !ap.params.empty() && ap.params[0] > 0 && ap.params[0] < edit_diam) {
            edit_diam = ap.params[0]; edit_ap = id;
        }
    }
    if (edit_ap >= 0) {
        double dm = to_mm(edit_diam, units);
        s << "  <!-- CALQUE EDIT : dessinez vos modifications ici -->\n"
          << "  <g id=\"edit-layer\" inkscape:groupmode=\"layer\""
          << " inkscape:label=\"EDIT (D"<<edit_ap<<" — "<<fmt4(dm)<<"mm)\">\n"
          << "  </g>\n\n";
    }

    s << "</svg>\n";
    return s.str();
}

// =========================================================================
//  SVG PARSING (paths, transforms, fill/stroke)
// =========================================================================

static std::vector<Polyline> cubic_bezier(Point p0, Point p1, Point p2, Point p3, int n=8) {
    Polyline pts;
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n;
        double u = 1-t;
        double x = u*u*u*p0.first + 3*u*u*t*p1.first + 3*u*t*t*p2.first + t*t*t*p3.first;
        double y = u*u*u*p0.second + 3*u*u*t*p1.second + 3*u*t*t*p2.second + t*t*t*p3.second;
        pts.push_back({x,y});
    }
    return {pts};
}

static Polyline quad_bezier(Point p0, Point p1, Point p2, int n=8) {
    Polyline pts;
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n;
        double u = 1-t;
        double x = u*u*p0.first + 2*u*t*p1.first + t*t*p2.first;
        double y = u*u*p0.second + 2*u*t*p1.second + t*t*p2.second;
        pts.push_back({x,y});
    }
    return pts;
}

static Polyline svg_arc_to_points(double x1, double y1, double rx, double ry,
                                   double phi, int fa, int fs, double x2, double y2) {
    if (rx == 0 || ry == 0) return {{x2,y2}};
    rx = std::abs(rx); ry = std::abs(ry);
    double pr = phi * PI / 180.0;
    double cp = cos(pr), sp = sin(pr);
    double dx2 = (x1-x2)/2, dy2 = (y1-y2)/2;
    double x1p = cp*dx2 + sp*dy2, y1p = -sp*dx2 + cp*dy2;
    double lam = x1p*x1p/(rx*rx) + y1p*y1p/(ry*ry);
    if (lam > 1) { rx *= sqrt(lam); ry *= sqrt(lam); }
    double num = std::max(0.0, rx*rx*ry*ry - rx*rx*y1p*y1p - ry*ry*x1p*x1p);
    double den = rx*rx*y1p*y1p + ry*ry*x1p*x1p;
    if (den == 0) return {{x2,y2}};
    double sq = sqrt(num/den);
    if (fa == fs) sq = -sq;
    double cxp = sq*rx*y1p/ry, cyp = -sq*ry*x1p/rx;
    double cx = cp*cxp - sp*cyp + (x1+x2)/2;
    double cy = sp*cxp + cp*cyp + (y1+y2)/2;

    auto angle = [](double ux, double uy, double vx, double vy) {
        double n = sqrt(ux*ux+uy*uy)*sqrt(vx*vx+vy*vy);
        if (n == 0) return 0.0;
        double c = std::clamp((ux*vx+uy*vy)/n, -1.0, 1.0);
        double a = acos(c);
        if (ux*vy - uy*vx < 0) a = -a;
        return a;
    };
    double th1 = angle(1,0, (x1p-cxp)/rx, (y1p-cyp)/ry);
    double dth = angle((x1p-cxp)/rx,(y1p-cyp)/ry, (-x1p-cxp)/rx,(-y1p-cyp)/ry);
    if (fs==0 && dth>0) dth -= 2*PI;
    else if (fs==1 && dth<0) dth += 2*PI;

    Polyline pts;
    int steps = 16;
    for (int i = 1; i <= steps; i++) {
        double t = th1 + dth * i / steps;
        double px = cp*rx*cos(t) - sp*ry*sin(t) + cx;
        double py = sp*rx*cos(t) + cp*ry*sin(t) + cy;
        pts.push_back({px,py});
    }
    return pts;
}

// SVG path tokenizer
static std::vector<std::string> tokenize_path(const std::string& d) {
    std::vector<std::string> toks;
    std::regex re(R"([MmLlHhVvCcSsQqTtAaZz]|[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)");
    auto begin = std::sregex_iterator(d.begin(), d.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; it++) toks.push_back(it->str());
    return toks;
}

static std::vector<Polyline> parse_svg_path_d(const std::string& d) {
    auto toks = tokenize_path(d);
    std::vector<Polyline> polys;
    Polyline cur;
    double cx=0,cy=0, sx=0,sy=0;
    Point last_cp = {0,0};
    char last_cmd = 0;
    size_t i = 0;

    auto nf = [&]() -> double { return (i < toks.size()) ? std::stod(toks[i++]) : 0; };
    auto ni = [&]() -> int { return (i < toks.size()) ? (int)std::stod(toks[i++]) : 0; };

    while (i < toks.size()) {
        char cmd;
        if (isalpha(toks[i][0]) && toks[i][0] != 'e' && toks[i][0] != 'E') {
            cmd = toks[i++][0];
        } else {
            cmd = last_cmd;
            if (cmd == 'M') cmd = 'L';
            if (cmd == 'm') cmd = 'l';
        }

        if (cmd == 'Z' || cmd == 'z') {
            if (cur.size() > 1) cur.push_back({sx,sy});
            cx=sx; cy=sy; last_cmd=cmd; continue;
        }
        if (cmd=='M') { if (!cur.empty()) polys.push_back(cur); cx=nf(); cy=nf(); sx=cx; sy=cy; cur={{cx,cy}}; }
        else if (cmd=='m') { if (!cur.empty()) polys.push_back(cur); cx+=nf(); cy+=nf(); sx=cx; sy=cy; cur={{cx,cy}}; }
        else if (cmd=='L') { cx=nf(); cy=nf(); cur.push_back({cx,cy}); }
        else if (cmd=='l') { cx+=nf(); cy+=nf(); cur.push_back({cx,cy}); }
        else if (cmd=='H') { cx=nf(); cur.push_back({cx,cy}); }
        else if (cmd=='h') { cx+=nf(); cur.push_back({cx,cy}); }
        else if (cmd=='V') { cy=nf(); cur.push_back({cx,cy}); }
        else if (cmd=='v') { cy+=nf(); cur.push_back({cx,cy}); }
        else if (cmd=='C') {
            double x1=nf(),y1=nf(),x2=nf(),y2=nf(),x=nf(),y=nf();
            auto pts=cubic_bezier({cx,cy},{x1,y1},{x2,y2},{x,y})[0];
            for(auto&p:pts) cur.push_back(p);
            last_cp={x2,y2}; cx=x; cy=y;
        } else if (cmd=='c') {
            double x1=cx+nf(),y1=cy+nf(),x2=cx+nf(),y2=cy+nf(),x=cx+nf(),y=cy+nf();
            auto pts=cubic_bezier({cx,cy},{x1,y1},{x2,y2},{x,y})[0];
            for(auto&p:pts) cur.push_back(p);
            last_cp={x2,y2}; cx=x; cy=y;
        } else if (cmd=='S') {
            double x1=cx,y1=cy;
            if(strchr("CcSs",last_cmd)){x1=2*cx-last_cp.first;y1=2*cy-last_cp.second;}
            double x2=nf(),y2=nf(),x=nf(),y=nf();
            auto pts=cubic_bezier({cx,cy},{x1,y1},{x2,y2},{x,y})[0];
            for(auto&p:pts) cur.push_back(p);
            last_cp={x2,y2}; cx=x; cy=y;
        } else if (cmd=='s') {
            double x1=cx,y1=cy;
            if(strchr("CcSs",last_cmd)){x1=2*cx-last_cp.first;y1=2*cy-last_cp.second;}
            double x2=cx+nf(),y2=cy+nf(),x=cx+nf(),y=cy+nf();
            auto pts=cubic_bezier({cx,cy},{x1,y1},{x2,y2},{x,y})[0];
            for(auto&p:pts) cur.push_back(p);
            last_cp={x2,y2}; cx=x; cy=y;
        } else if (cmd=='Q') {
            double x1=nf(),y1=nf(),x=nf(),y=nf();
            auto pts=quad_bezier({cx,cy},{x1,y1},{x,y});
            for(auto&p:pts) cur.push_back(p);
            last_cp={x1,y1}; cx=x; cy=y;
        } else if (cmd=='q') {
            double x1=cx+nf(),y1=cy+nf(),x=cx+nf(),y=cy+nf();
            auto pts=quad_bezier({cx,cy},{x1,y1},{x,y});
            for(auto&p:pts) cur.push_back(p);
            last_cp={x1,y1}; cx=x; cy=y;
        } else if (cmd=='A') {
            double rx=nf(),ry=nf(),phi=nf(); int fa=ni(),fs=ni(); double x=nf(),y=nf();
            auto pts=svg_arc_to_points(cx,cy,rx,ry,phi,fa,fs,x,y);
            for(auto&p:pts) cur.push_back(p); cx=x; cy=y;
        } else if (cmd=='a') {
            double rx=nf(),ry=nf(),phi=nf(); int fa=ni(),fs=ni(); double dx=nf(),dy=nf();
            double x=cx+dx,y=cy+dy;
            auto pts=svg_arc_to_points(cx,cy,rx,ry,phi,fa,fs,x,y);
            for(auto&p:pts) cur.push_back(p); cx=x; cy=y;
        } else { i++; }

        last_cmd = cmd;
    }
    if (!cur.empty()) polys.push_back(cur);
    return polys;
}

// =========================================================================
//  SVG XML minimal parser
// =========================================================================

struct XmlAttr { std::string name, value; };
struct XmlNode {
    std::string tag;
    std::vector<XmlAttr> attrs;
    std::vector<XmlNode> children;
    std::string get(const std::string& name) const {
        for (auto& a : attrs) if (a.name == name) return a.value;
        return "";
    }
};

static size_t skip_xml_ws(const std::string& s, size_t i) {
    while (i < s.size() && isspace(s[i])) i++;
    return i;
}

static XmlNode parse_xml_node(const std::string& s, size_t& i);

static std::string parse_xml_attr_val(const std::string& s, size_t& i) {
    char q = s[i++]; // " or '
    std::string v;
    while (i < s.size() && s[i] != q) v += s[i++];
    if (i < s.size()) i++;
    return v;
}

static std::vector<XmlNode> parse_xml_children(const std::string& s, size_t& i, const std::string& parent_tag);

static XmlNode parse_xml_node(const std::string& s, size_t& i) {
    XmlNode node;
    i++; // skip <
    // tag name
    while (i < s.size() && !isspace(s[i]) && s[i] != '>' && s[i] != '/') node.tag += s[i++];
    // attrs
    while (i < s.size() && s[i] != '>' && s[i] != '/') {
        i = skip_xml_ws(s, i);
        if (s[i] == '>' || s[i] == '/') break;
        XmlAttr attr;
        while (i < s.size() && s[i] != '=' && !isspace(s[i]) && s[i] != '>') attr.name += s[i++];
        i = skip_xml_ws(s, i);
        if (i < s.size() && s[i] == '=') {
            i++; i = skip_xml_ws(s, i);
            if (i < s.size() && (s[i] == '"' || s[i] == '\''))
                attr.value = parse_xml_attr_val(s, i);
        }
        if (!attr.name.empty()) node.attrs.push_back(attr);
    }
    if (i < s.size() && s[i] == '/') { i++; if (i<s.size()&&s[i]=='>') i++; return node; }
    if (i < s.size() && s[i] == '>') i++;
    // children
    node.children = parse_xml_children(s, i, node.tag);
    return node;
}

static std::vector<XmlNode> parse_xml_children(const std::string& s, size_t& i, const std::string& parent_tag) {
    std::vector<XmlNode> children;
    while (i < s.size()) {
        i = skip_xml_ws(s, i);
        if (i >= s.size()) break;
        if (s[i] == '<') {
            if (i+1 < s.size() && s[i+1] == '/') {
                // closing tag
                while (i < s.size() && s[i] != '>') i++;
                if (i < s.size()) i++;
                return children;
            }
            if (i+1 < s.size() && (s[i+1] == '?' || s[i+1] == '!')) {
                // skip <?...?> and <!--...-->
                if (i+3 < s.size() && s[i+1]=='!' && s[i+2]=='-' && s[i+3]=='-') {
                    auto end = s.find("-->", i);
                    i = (end == std::string::npos) ? s.size() : end + 3;
                } else {
                    while (i < s.size() && s[i] != '>') i++;
                    if (i < s.size()) i++;
                }
                continue;
            }
            children.push_back(parse_xml_node(s, i));
        } else {
            while (i < s.size() && s[i] != '<') i++;
        }
    }
    return children;
}

static XmlNode parse_svg_file(const std::string& path) {
    std::string raw = read_file(path);
    size_t i = 0;
    // Find first <svg
    while (i < raw.size()) {
        i = skip_xml_ws(raw, i);
        if (i < raw.size() && raw[i] == '<') {
            if (raw.substr(i, 4) == "<svg" || raw.substr(i, 4) == "<SVG")
                return parse_xml_node(raw, i);
            if (raw[i+1] == '?' || raw[i+1] == '!') {
                if (raw.substr(i, 4) == "<!--") {
                    auto end = raw.find("-->", i);
                    i = (end == std::string::npos) ? raw.size() : end+3;
                } else {
                    while (i < raw.size() && raw[i] != '>') i++;
                    if (i < raw.size()) i++;
                }
            } else break;
        } else i++;
    }
    return parse_xml_node(raw, i);
}

// =========================================================================
//  SVG element extraction with fill/stroke/holes
// =========================================================================

using Matrix = std::array<double, 6>; // a b c d e f

static Matrix identity_matrix() { return {1,0,0,1,0,0}; }

static Matrix multiply(const Matrix& m1, const Matrix& m2) {
    return {
        m1[0]*m2[0]+m1[2]*m2[1], m1[1]*m2[0]+m1[3]*m2[1],
        m1[0]*m2[2]+m1[2]*m2[3], m1[1]*m2[2]+m1[3]*m2[3],
        m1[0]*m2[4]+m1[2]*m2[5]+m1[4], m1[1]*m2[4]+m1[3]*m2[5]+m1[5]
    };
}

static Point apply_matrix(const Matrix& m, double x, double y) {
    return {m[0]*x+m[2]*y+m[4], m[1]*x+m[3]*y+m[5]};
}

static Matrix parse_transform(const std::string& ts) {
    Matrix r = identity_matrix();
    if (ts.empty()) return r;
    std::regex re(R"((\w+)\s*\(([^)]+)\))");
    auto begin = std::sregex_iterator(ts.begin(), ts.end(), re);
    for (auto it = begin; it != std::sregex_iterator(); it++) {
        std::string func = (*it)[1];
        std::string args_s = (*it)[2];
        std::regex num_re(R"([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)");
        std::vector<double> vals;
        auto nb = std::sregex_iterator(args_s.begin(), args_s.end(), num_re);
        for (auto n = nb; n != std::sregex_iterator(); n++) vals.push_back(std::stod(n->str()));

        if (func == "matrix" && vals.size() >= 6) r = {vals[0],vals[1],vals[2],vals[3],vals[4],vals[5]};
        else if (func == "translate") {
            double tx = vals.size()>0?vals[0]:0, ty = vals.size()>1?vals[1]:0;
            r = multiply(r, {1,0,0,1,tx,ty});
        } else if (func == "scale") {
            double sx = vals.size()>0?vals[0]:1, sy = vals.size()>1?vals[1]:sx;
            r = multiply(r, {sx,0,0,sy,0,0});
        } else if (func == "rotate" && !vals.empty()) {
            double a = vals[0]*PI/180; double ca=cos(a),sa=sin(a);
            r = multiply(r, {ca,sa,-sa,ca,0,0});
        }
    }
    return r;
}

static bool has_transform(const Matrix& m) {
    return !(m[0]==1&&m[1]==0&&m[2]==0&&m[3]==1&&m[4]==0&&m[5]==0);
}

static std::pair<bool,bool> detect_fill_stroke(const XmlNode& n) {
    std::string style = n.get("style");
    std::string fill_v, stroke_v;
    for (auto& part : split(style, ';')) {
        auto p = trim(part);
        if (p.substr(0,5) == "fill:") fill_v = trim(p.substr(5));
        if (p.substr(0,7) == "stroke:") stroke_v = trim(p.substr(7));
    }
    if (fill_v.empty()) fill_v = n.get("fill");
    if (stroke_v.empty()) stroke_v = n.get("stroke");
    bool has_fill = !fill_v.empty() && to_lower(fill_v) != "none";
    if (fill_v.empty()) has_fill = true; // SVG default
    bool has_stroke = !stroke_v.empty() && to_lower(stroke_v) != "none";
    return {has_fill, has_stroke};
}

static double signed_area(const Polyline& p) {
    double area = 0;
    for (size_t i = 0; i < p.size(); i++) {
        auto [x1,y1] = p[i];
        auto [x2,y2] = p[(i+1)%p.size()];
        area += (x2-x1)*(y2+y1);
    }
    return area/2;
}

static std::vector<Polyline> extract_polylines(const XmlNode& n, const Matrix& parent) {
    Matrix m = parent;
    std::string ts = n.get("transform");
    if (!ts.empty()) m = multiply(parent, parse_transform(ts));

    std::vector<Polyline> result;
    std::string tag = n.tag;
    // Strip namespace
    auto colon = tag.find(':');
    if (colon != std::string::npos) tag = tag.substr(colon+1);
    auto ns = tag.find('}');
    if (ns != std::string::npos) tag = tag.substr(ns+1);

    if (tag == "path") {
        std::string d = n.get("d");
        if (!d.empty()) {
            auto polys = parse_svg_path_d(d);
            for (auto& pl : polys) {
                if (has_transform(m))
                    for (auto& [x,y] : pl) std::tie(x,y) = apply_matrix(m,x,y);
                if (pl.size() >= 2) result.push_back(pl);
            }
        }
    } else if (tag == "polygon") {
        std::string pts_s = n.get("points");
        std::regex num_re(R"([+-]?(?:\d+\.?\d*|\.\d+))");
        std::vector<double> coords;
        auto b = std::sregex_iterator(pts_s.begin(), pts_s.end(), num_re);
        for (auto it=b; it!=std::sregex_iterator(); it++) coords.push_back(std::stod(it->str()));
        Polyline pl;
        for (size_t j=0; j+1<coords.size(); j+=2) {
            double x=coords[j], y=coords[j+1];
            if (has_transform(m)) std::tie(x,y) = apply_matrix(m,x,y);
            pl.push_back({x,y});
        }
        if (pl.size()>=2) { pl.push_back(pl[0]); result.push_back(pl); }
    } else if (tag == "rect" && n.get("id") != "background") {
        double x=std::stod(n.get("x").empty()?"0":n.get("x"));
        double y=std::stod(n.get("y").empty()?"0":n.get("y"));
        double w=std::stod(n.get("width").empty()?"0":n.get("width"));
        double h=std::stod(n.get("height").empty()?"0":n.get("height"));
        Polyline pl = {{x,y},{x+w,y},{x+w,y+h},{x,y+h},{x,y}};
        if (has_transform(m)) for(auto&[px,py]:pl) std::tie(px,py)=apply_matrix(m,px,py);
        result.push_back(pl);
    } else if (tag == "circle") {
        double cx_v=std::stod(n.get("cx").empty()?"0":n.get("cx"));
        double cy_v=std::stod(n.get("cy").empty()?"0":n.get("cy"));
        double r=std::stod(n.get("r").empty()?"0":n.get("r"));
        if (r > 0) {
            Polyline pl;
            for (int j=0; j<=32; j++) {
                double a = 2*PI*j/32;
                double px=cx_v+r*cos(a), py=cy_v+r*sin(a);
                if (has_transform(m)) std::tie(px,py)=apply_matrix(m,px,py);
                pl.push_back({px,py});
            }
            result.push_back(pl);
        }
    }
    return result;
}

struct FillStrokeResult {
    std::vector<Polyline> fills, holes, strokes;
};

static FillStrokeResult extract_with_fill(const XmlNode& n, const Matrix& parent = identity_matrix()) {
    auto polys = extract_polylines(n, parent);
    auto [hf, hs] = detect_fill_stroke(n);
    FillStrokeResult r;
    if (hf && polys.size() > 1) {
        std::vector<std::pair<double,Polyline*>> areas;
        for (auto& pl : polys) areas.push_back({signed_area(pl), &pl});
        double max_abs = 0; int outer_sign = 1;
        for (auto& [a,p] : areas) if (std::abs(a) > max_abs) { max_abs=std::abs(a); outer_sign=(a>=0?1:-1); }
        for (auto& [a,p] : areas) {
            if (a == 0 || (a>0)==(outer_sign>0)) r.fills.push_back(*p);
            else r.holes.push_back(*p);
        }
    } else if (hf) r.fills = polys;
    if (hs) r.strokes = polys;
    if (!hf && !hs) r.fills = polys;
    return r;
}

static FillStrokeResult extract_group_recursive(const XmlNode& g, const Matrix& parent = identity_matrix()) {
    Matrix m = parent;
    std::string ts = g.get("transform");
    if (!ts.empty()) m = multiply(parent, parse_transform(ts));
    FillStrokeResult r;
    for (auto& child : g.children) {
        std::string tag = child.tag;
        auto ns = tag.find('}'); if (ns!=std::string::npos) tag=tag.substr(ns+1);
        if (tag == "g") {
            auto cr = extract_group_recursive(child, m);
            r.fills.insert(r.fills.end(), cr.fills.begin(), cr.fills.end());
            r.holes.insert(r.holes.end(), cr.holes.begin(), cr.holes.end());
            r.strokes.insert(r.strokes.end(), cr.strokes.begin(), cr.strokes.end());
        } else {
            auto cr = extract_with_fill(child, m);
            r.fills.insert(r.fills.end(), cr.fills.begin(), cr.fills.end());
            r.holes.insert(r.holes.end(), cr.holes.begin(), cr.holes.end());
            r.strokes.insert(r.strokes.end(), cr.strokes.begin(), cr.strokes.end());
        }
    }
    return r;
}

// =========================================================================
//  SVG -> SvgElements
// =========================================================================

static void collect_groups(const XmlNode& node, std::vector<const XmlNode*>& groups) {
    std::string tag = node.tag;
    auto ns = tag.find('}'); if (ns!=std::string::npos) tag=tag.substr(ns+1);
    if (tag == "g") groups.push_back(&node);
    for (auto& c : node.children) collect_groups(c, groups);
}

static SvgElements parse_svg_elements(const std::string& svg_path, int edit_aperture) {
    XmlNode root = parse_svg_file(svg_path);
    SvgElements elems;

    std::vector<const XmlNode*> groups;
    collect_groups(root, groups);

    auto add_fhs = [&](int ap, const FillStrokeResult& r, const std::string& label) {
        if (!r.fills.empty()) {
            auto& v = elems.regions[ap]; v.insert(v.end(), r.fills.begin(), r.fills.end());
        }
        if (!r.holes.empty()) {
            auto& v = elems.regions_clear[ap]; v.insert(v.end(), r.holes.begin(), r.holes.end());
        }
        if (!r.strokes.empty()) {
            auto& v = elems.traces[ap]; v.insert(v.end(), r.strokes.begin(), r.strokes.end());
        }
        if (!label.empty() && (!r.fills.empty()||!r.holes.empty()||!r.strokes.empty()))
            std::cout << "  " << label << ": " << r.fills.size() << " fills + "
                      << r.holes.size() << " holes + " << r.strokes.size() << " strokes -> D" << ap << "\n";
    };

    for (auto* g : groups) {
        std::string gid = g->get("id");
        if (gid == "outline-ref") continue;

        // Edit layer
        if (gid == "edit-layer" && edit_aperture >= 0) {
            auto r = extract_group_recursive(*g);
            add_fhs(edit_aperture, r, "EDIT layer");
            continue;
        }

        // traces-Dxx
        std::regex traces_re(R"(traces-D(\d+))");
        std::smatch tm;
        if (std::regex_match(gid, tm, traces_re)) {
            int ap = std::stoi(tm[1]);
            for (auto& c : g->children) {
                auto polys = extract_polylines(c, identity_matrix());
                auto& v = elems.traces[ap]; v.insert(v.end(), polys.begin(), polys.end());
            }
            continue;
        }

        // flashes-Dxx
        std::regex flashes_re(R"(flashes-D(\d+))");
        if (std::regex_match(gid, tm, flashes_re)) {
            int ap = std::stoi(tm[1]);
            for (auto& c : g->children) {
                std::string tag = c.tag; auto ns=tag.find('}'); if(ns!=std::string::npos)tag=tag.substr(ns+1);
                if (tag=="circle") {
                    elems.flashes[ap].push_back({
                        std::stod(c.get("cx").empty()?"0":c.get("cx")),
                        std::stod(c.get("cy").empty()?"0":c.get("cy"))});
                } else if (tag=="rect" && c.get("id")!="background") {
                    double x=std::stod(c.get("x").empty()?"0":c.get("x"));
                    double y=std::stod(c.get("y").empty()?"0":c.get("y"));
                    double w=std::stod(c.get("width").empty()?"0":c.get("width"));
                    double h=std::stod(c.get("height").empty()?"0":c.get("height"));
                    elems.flashes[ap].push_back({x+w/2, y+h/2});
                } else if (tag=="polygon") {
                    std::string pts_s=c.get("points");
                    auto parts=split(trim(pts_s),' ');
                    double sx=0,sy=0; int n=0;
                    for(auto&p:parts){auto xy=split(p,',');if(xy.size()>=2){sx+=std::stod(xy[0]);sy+=std::stod(xy[1]);n++;}}
                    if(n) elems.flashes[ap].push_back({sx/n,sy/n});
                }
            }
            continue;
        }

        // aperture-Dxx fallback
        std::regex ap_re(R"(aperture-D(\d+))");
        if (std::regex_match(gid, tm, ap_re)) {
            int ap = std::stoi(tm[1]);
            int fallback_ap = edit_aperture >= 0 ? edit_aperture : ap;
            for (auto& c : g->children) {
                std::string cid = c.get("id");
                if (cid.substr(0,8)=="traces-D"||cid.substr(0,9)=="flashes-D") continue;
                auto r = extract_with_fill(c);
                add_fhs(fallback_ap, r, "[compat] D"+std::to_string(ap)+"/"+cid);
            }
        }
    }
    return elems;
}

// =========================================================================
//  GERBER GENERATION (SVG -> Gerber)
// =========================================================================

static long mm_to_gerber(double mm, const std::string& units, int n_dec) {
    double v = (units == "inch") ? mm * MM_TO_INCH : mm;
    return std::lround(v * std::pow(10.0, n_dec));
}

static std::string fmt_coord(long val, int n_int, int n_dec) {
    int total = n_int + n_dec;
    char buf[32];
    snprintf(buf, sizeof(buf), "%0*ld", total, std::abs(val));
    return (val < 0 ? "-" : "") + std::string(buf);
}

static std::string generate_gerber(const GerberInfo& info, const SvgElements& elems) {
    auto& fmt = info.format;
    auto& units = info.units;
    std::ostringstream out;

    // Header
    out << "G75*\n";
    out << (units=="inch" ? "%MOIN*%\n" : "%MOMM*%\n");
    out << "%OFA0B0*%\n";
    out << "%FS" << fmt.zero_omit << fmt.coord_mode
        << "X" << fmt.x_int << fmt.x_dec << "Y" << fmt.y_int << fmt.y_dec << "*%\n";
    out << "%IPPOS*%\n%LPD*%\n";

    for (auto& [name, body] : info.macros) {
        out << "%AM" << name << "*\n";
        for (auto& b : body) out << b << "*\n";
        out << "%\n";
    }
    for (auto& [id, ap] : info.apertures) {
        std::string ps;
        if (!ap.params_raw.empty()) { for(size_t i=0;i<ap.params_raw.size();i++){if(i)ps+="X";ps+=ap.params_raw[i];} }
        else { for(size_t i=0;i<ap.params.size();i++){if(i)ps+="X";ps+=std::to_string(ap.params[i]);} }
        out << "%ADD" << id << ap.type << (ps.empty()?"":",")<< ps << "*%\n";
    }

    // Collect all aperture IDs
    std::set<int> all_ids;
    for(auto&[k,v]:elems.traces) all_ids.insert(k);
    for(auto&[k,v]:elems.flashes) all_ids.insert(k);
    for(auto&[k,v]:elems.regions) all_ids.insert(k);
    for(auto&[k,v]:elems.regions_clear) all_ids.insert(k);

    auto emit_coord = [&](double xmm, double ymm, int dcode) {
        ymm = -ymm;
        long gx = mm_to_gerber(xmm, units, fmt.x_dec);
        long gy = mm_to_gerber(ymm, units, fmt.y_dec);
        out << "X" << fmt_coord(gx, fmt.x_int, fmt.x_dec)
            << "Y" << fmt_coord(gy, fmt.y_int, fmt.y_dec)
            << "D0" << dcode << "*\n";
    };

    auto emit_region = [&](const Polyline& pl) {
        if (pl.size() < 3) return;
        out << "G36*\n";
        emit_coord(pl[0].first, pl[0].second, 2);
        for (size_t i=1; i<pl.size(); i++) emit_coord(pl[i].first, pl[i].second, 1);
        if (std::abs(pl[0].first-pl.back().first)>0.001 || std::abs(pl[0].second-pl.back().second)>0.001)
            emit_coord(pl[0].first, pl[0].second, 1);
        out << "G37*\n";
    };

    for (int ap_id : all_ids) {
        out << "D" << ap_id << "*\n";

        if (elems.traces.count(ap_id))
            for (auto& pl : elems.traces.at(ap_id)) {
                if (pl.size()<2) continue;
                emit_coord(pl[0].first, pl[0].second, 2);
                for (size_t i=1;i<pl.size();i++) emit_coord(pl[i].first, pl[i].second, 1);
            }

        if (elems.flashes.count(ap_id))
            for (auto& [fx,fy] : elems.flashes.at(ap_id)) emit_coord(fx, fy, 3);

        if (elems.regions.count(ap_id))
            for (auto& pl : elems.regions.at(ap_id)) emit_region(pl);

        if (elems.regions_clear.count(ap_id)) {
            out << "%LPC*%\n";
            for (auto& pl : elems.regions_clear.at(ap_id)) emit_region(pl);
            out << "%LPD*%\n";
        }
    }
    out << "M02*\n";
    return out.str();
}

// =========================================================================
//  COMMANDS
// =========================================================================

static void cmd_svg(int argc, char** argv) {
    std::string outline_path;
    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) {
        if (std::string(argv[i]) == "--outline" && i+1 < argc) {
            outline_path = argv[++i];
        } else {
            args.push_back(argv[i]);
        }
    }
    if (args.empty()) {
        std::cerr << "Usage: gerber_tool --svg input.GTO [output.svg] [--outline board.GKO]\n";
        exit(1);
    }

    std::string input = args[0];
    std::string input_name = basename(input);
    std::string output_svg = args.size() > 1 ? args[1] : input_name + ".svg";
    std::string output_json = output_svg.substr(0, output_svg.size()-4) + ".gerber_info.json";

    std::cout << "Parsing " << input << "...\n";
    GerberInfo info = parse_gerber(input);
    int nlines=0, nflash=0;
    for (auto& op : info.operations) { if (op.type=="line") nlines++; else nflash++; }
    std::cout << "  Units: " << info.units << "\n"
              << "  Apertures: " << info.apertures.size() << "\n"
              << "  Operations: " << info.operations.size()
              << " (" << nlines << " lines, " << nflash << " flashes)\n";

    std::vector<Operation>* ol_ops = nullptr;
    GerberInfo ol_info;
    if (!outline_path.empty()) {
        std::cout << "Parsing outline " << outline_path << "...\n";
        ol_info = parse_gerber(outline_path);
        ol_ops = &ol_info.operations;
        std::cout << "  Outline: " << ol_ops->size() << " operations\n";
    }

    // Find edit aperture
    int edit_ap = -1; double edit_d = 1e9;
    for (auto& [id,ap] : info.apertures)
        if (ap.type=="C" && !ap.params.empty() && ap.params[0]>0 && ap.params[0]<edit_d)
            { edit_d=ap.params[0]; edit_ap=id; }
    info.edit_aperture = edit_ap;

    std::cout << "Generating SVG...\n";
    std::string svg = generate_svg(info, ol_ops, ol_ops ? &ol_info : nullptr);
    write_file(output_svg, svg);
    std::cout << "  -> " << output_svg << " (" << svg.size() << " bytes)\n";

    std::string json = write_json(info);
    write_file(output_json, json);
    std::cout << "  -> " << output_json << " (" << json.size() << " bytes)\n";
    std::cout << "Done!\n";
}

static void cmd_gerber(int argc, char** argv) {
    if (argc < 1) {
        std::cerr << "Usage: gerber_tool --gerber board.GTO.svg [output.GTO]\n";
        exit(1);
    }

    std::string svg_path = argv[0];
    std::string base = ends_with(to_lower(svg_path), ".svg") ? svg_path.substr(0, svg_path.size()-4) : svg_path;
    std::string json_path = base + ".gerber_info.json";

    {   std::ifstream test(json_path);
        if (!test) {
            std::cerr << "Erreur: " << json_path << " introuvable.\n"
                      << "  Ce fichier est genere par gerber_tool --svg\n";
            exit(1);
        }
    }

    std::string output;
    if (argc > 1) { output = argv[1]; }
    else {
        auto dot = base.rfind('.');
        if (dot != std::string::npos)
            output = base.substr(0, dot) + "_EDIT" + base.substr(dot);
        else output = base + "_EDIT.gbr";
    }

    std::cout << "Loading info from " << json_path << "...\n";
    GerberInfo info = read_json_info(json_path);
    std::cout << "  Source: " << info.source_file << "\n"
              << "  Units: " << info.units << "\n"
              << "  Apertures: " << info.apertures.size() << "\n";

    std::cout << "Parsing SVG " << svg_path << "...\n";
    SvgElements elems = parse_svg_elements(svg_path, info.edit_aperture);

    int nt=0,nf=0,nr=0,nh=0;
    for(auto&[k,v]:elems.traces) nt+=v.size();
    for(auto&[k,v]:elems.flashes) nf+=v.size();
    for(auto&[k,v]:elems.regions) nr+=v.size();
    for(auto&[k,v]:elems.regions_clear) nh+=v.size();
    std::cout << "  Traces: " << nt << "\n  Flashes: " << nf << "\n";
    if (nr) std::cout << "  Regions (fill): " << nr << "\n";
    if (nh) std::cout << "  Regions (holes): " << nh << "\n";

    std::cout << "Generating Gerber...\n";
    std::string gbr = generate_gerber(info, elems);
    write_file(output, gbr);
    std::cout << "  -> " << output << " (" << gbr.size() << " bytes)\n";
    std::cout << "Done!\n";
}

// =========================================================================
//  MAIN
// =========================================================================

static void print_help(const char* prog) {
    std::cout << "Usage: " << prog << " --svg  board.GTO [output.svg] [--outline board.GKO]\n"
              << "       " << prog << " --gerber board.GTO.svg [output.GTO]\n\n"
              << "Modes :\n"
              << "  --svg     Gerber -> SVG + JSON   (pour edition dans Inkscape)\n"
              << "  --gerber  SVG -> Gerber          (reconversion apres edition)\n\n"
              << "Extensions Gerber courantes (Eagle / KiCad) :\n"
              << "  .GTL  Top Copper          .GBL  Bottom Copper\n"
              << "  .GTO  Top Silkscreen      .GBO  Bottom Silkscreen\n"
              << "  .GTS  Top Soldermask      .GBS  Bottom Soldermask\n"
              << "  .GTP  Top Paste           .GBP  Bottom Paste\n"
              << "  .GKO  Board Outline       .GML  Milling Layer (= GKO)\n\n"
              << "Options (--svg) :\n"
              << "  --outline board.GKO   Contour PCB en calque verrouille\n";
}

int main(int argc, char** argv) {
    if (argc < 2) { print_help(argv[0]); return 0; }
    std::string mode = argv[1];
    if (mode == "-h" || mode == "--help") { print_help(argv[0]); return 0; }
    if (mode == "--svg") { cmd_svg(argc-2, argv+2); return 0; }
    if (mode == "--gerber") { cmd_gerber(argc-2, argv+2); return 0; }
    // Auto-detect
    if (ends_with(to_lower(mode), ".svg")) cmd_gerber(argc-1, argv+1);
    else cmd_svg(argc-1, argv+1);
    return 0;
}
