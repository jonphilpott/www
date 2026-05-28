#include "renderer.h"
#include <sstream>
#include <vector>
#include <regex>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <algorithm>

// =============================================================================
// Helpers
// =============================================================================

std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else if (c == ' ') {
            out += '+';
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// Trim leading/trailing whitespace from a string
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Scrub markdown noise that small models frequently add:
//   **TITLE:** → TITLE:    (bold markers)
//   1. TITLE:  → TITLE:    (numbered list prefix)
static std::string scrub_line(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '*') continue;   // strip all asterisks
        if (s[i] == '_') continue;   // strip underscores (italic)
        out += s[i];
    }
    out = trim(out);

    // Strip a leading numeric list marker like "1." or "1)" or "Result 1:"
    size_t i = 0;
    while (i < out.size() && std::isdigit(static_cast<unsigned char>(out[i]))) i++;
    if (i > 0 && i < out.size() && (out[i] == '.' || out[i] == ')' || out[i] == ':')) {
        out = trim(out.substr(i + 1));
    }

    return out;
}

// Case-insensitive field extraction, tolerant of markdown and numbered prefixes.
// e.g. all of these match key="TITLE":
//   "TITLE: Foo"   "title: Foo"   "**TITLE:** Foo"   "1. TITLE: Foo"
static std::string extract_field(const std::string& raw_line, const std::string& key) {
    std::string line = scrub_line(raw_line);

    // Build lowercase versions for comparison
    std::string ll = line, lk = key + ":";
    std::transform(ll.begin(), ll.end(), ll.begin(), ::tolower);
    std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);

    if (ll.size() > lk.size() && ll.substr(0, lk.size()) == lk) {
        return trim(line.substr(lk.size()));
    }
    return "";
}

// Replace !!marked text!! with red italic HTML.
// The model is instructed to wrap intentionally fabricated facts in !!...!!
// so we can highlight them without relying on the model to judge its own accuracy.
static std::string highlight_false_facts(const std::string& html) {
    // Non-greedy match between !! pairs. The (.+?) captures the false fact text.
    // We use std::regex_replace with a format string that wraps it in FONT+I tags.
    std::regex pattern(R"(!!(.+?)!!)");
    return std::regex_replace(html, pattern,
        R"(<FONT COLOR="#FF0000"><I>$1</I></FONT>)");
}

// Strip any text the model may have output before the actual <HTML tag.
// Small models sometimes write a sentence like "Here is the page:" before
// starting the HTML; we don't want that appearing in the browser.
static std::string strip_preamble(const std::string& s) {
    // Case-insensitive search for <html or <HTML
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    size_t pos = lower.find("<html");
    if (pos != std::string::npos) return s.substr(pos);
    return s;  // no <html found — return as-is and let the browser cope
}

// =============================================================================
// Fix 1: encode < > & inside <PRE> blocks so code renders correctly.
//
// Models often write raw C/HTML/Perl inside PRE tags, meaning tags like
// <stdio.h> are swallowed by the browser. We decode any partial entities
// first (so we never double-encode), then re-encode cleanly.
// =============================================================================

static std::string decode_html_entities(const std::string& s) {
    // Simple decode for the entities that appear in code.
    // Process in an order that avoids replacing the & in "&amp;" before we
    // expand the other entities — so &amp; must be decoded last.
    static const std::pair<const char*, const char*> ENTITIES[] = {
        {"&lt;",   "<"},
        {"&gt;",   ">"},
        {"&quot;", "\""},
        {"&apos;", "'"},
        {"&amp;",  "&"},  // must be last
    };
    std::string r = s;
    for (auto& [enc, raw] : ENTITIES) {
        size_t pos = 0;
        while ((pos = r.find(enc, pos)) != std::string::npos) {
            r.replace(pos, strlen(enc), raw);
            pos += strlen(raw);
        }
    }
    return r;
}

static std::string encode_html_text(const std::string& s) {
    std::string r;
    r.reserve(s.size() * 2);
    for (unsigned char c : s) {
        if      (c == '&') r += "&amp;";
        else if (c == '<') r += "&lt;";
        else if (c == '>') r += "&gt;";
        else               r += static_cast<char>(c);
    }
    return r;
}

static std::string fix_code_blocks(const std::string& html) {
    // Match <pre ...> ... </pre> including multiline content ([\s\S]*?).
    // icase handles <PRE>, <Pre>, <pre> all the same.
    std::regex pat(R"(<pre(\s[^>]*)?>([\s\S]*?)</pre>)",
                   std::regex_constants::icase);
    std::string result;
    size_t last = 0;

    auto it  = std::sregex_iterator(html.begin(), html.end(), pat);
    auto end = std::sregex_iterator();
    for (; it != end; ++it) {
        const auto& m = *it;
        result += html.substr(last, m.position() - last);

        // m[0] = full match, m[1] = optional attrs, m[2] = inner content
        std::string open_tag = "<PRE" + m[1].str() + ">";
        std::string content  = decode_html_entities(m[2].str());
        result += open_tag + encode_html_text(content) + "</PRE>";

        last = m.position() + m.length();
    }
    result += html.substr(last);
    return result;
}

// =============================================================================
// Fix 2: ensure readable contrast between BODY BGCOLOR and TEXT.
//
// The model sometimes picks two dark (or two light) colours. We calculate
// relative luminance per WCAG 2.1 and, if contrast < 3:1, replace the TEXT
// colour with whichever of black or white gives better contrast against BGCOLOR.
// 3:1 is intentionally lenient — we want garish, not accessible, just readable.
// =============================================================================

static bool parse_hex_color(const std::string& s, int& r, int& g, int& b) {
    std::string h = s;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() == 3)
        h = {h[0],h[0], h[1],h[1], h[2],h[2]};  // expand #RGB → #RRGGBB
    if (h.size() != 6) return false;
    try {
        r = std::stoi(h.substr(0, 2), nullptr, 16);
        g = std::stoi(h.substr(2, 2), nullptr, 16);
        b = std::stoi(h.substr(4, 2), nullptr, 16);
        return true;
    } catch (...) { return false; }
}

// WCAG 2.1 relative luminance: linearise sRGB then apply the weighting
static double relative_luminance(int r, int g, int b) {
    auto lin = [](double c) {
        c /= 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
}

static double contrast_ratio(int r1, int g1, int b1, int r2, int g2, int b2) {
    double l1 = relative_luminance(r1, g1, b1);
    double l2 = relative_luminance(r2, g2, b2);
    if (l1 < l2) std::swap(l1, l2);
    return (l1 + 0.05) / (l2 + 0.05);
}

std::string fix_color_contrast(const std::string& html) {
    // Find the BODY tag and pull out BGCOLOR and TEXT attribute values
    std::regex body_pat(R"(<body([^>]*)>)", std::regex_constants::icase);
    std::smatch bm;
    if (!std::regex_search(html, bm, body_pat)) return html;

    std::string attrs = bm[1].str();

    auto extract_attr = [&attrs](const std::string& name) -> std::string {
        std::regex ap(name + "\\s*=\\s*\"([^\"]*)\"", std::regex_constants::icase);
        std::smatch am;
        return std::regex_search(attrs, am, ap) ? am[1].str() : "";
    };

    std::string bg_str   = extract_attr("bgcolor");
    std::string text_str = extract_attr("text");
    if (bg_str.empty() || text_str.empty()) return html;

    int br, bg, bb, tr, tg, tb;
    if (!parse_hex_color(bg_str, br, bg, bb)) return html;
    if (!parse_hex_color(text_str, tr, tg, tb)) return html;

    if (contrast_ratio(br, bg, bb, tr, tg, tb) >= 3.0) return html;  // already fine

    // Pick whichever of black or white contrasts better against the background
    double vs_white = contrast_ratio(br, bg, bb, 255, 255, 255);
    double vs_black = contrast_ratio(br, bg, bb, 0,   0,   0);
    std::string new_text = (vs_white >= vs_black) ? "#FFFFFF" : "#000000";

    // Replace just the TEXT attribute value inside the BODY tag
    std::string old_body = bm[0].str();
    std::regex text_val_pat(R"(((?:text)\s*=\s*")[^"]*")", std::regex_constants::icase);
    std::string new_body = std::regex_replace(old_body, text_val_pat,
                                               "$1" + new_text + "\"");

    // Splice the fixed BODY tag back in
    std::string result = html;
    result.replace(bm.position(), bm.length(), new_body);
    return result;
}

// =============================================================================
// Chat response post-processing helpers
// =============================================================================

// Models trained on markdown emit triple-backtick fences even when asked to
// use HTML. This converts ``` blocks to <PRE>...</PRE> with the content
// HTML-encoded — so #include <stdio.h> becomes #include &lt;stdio.h&gt; and
// the browser renders it as text rather than eating it as an unknown tag.
static std::string convert_markdown_code_fences(const std::string& s) {
    std::string result;
    std::istringstream ss(s);
    std::string line;
    bool in_fence = false;
    std::string fence_content;

    while (std::getline(ss, line)) {
        if (!in_fence) {
            if (line.size() >= 3 && line.substr(0, 3) == "```") {
                in_fence = true;
                fence_content.clear();
                // rest of the opening line is a language hint — discard it
            } else {
                result += line + "\n";
            }
        } else {
            if (line.size() >= 3 && line.substr(0, 3) == "```") {
                in_fence = false;
                result += "<PRE>" + encode_html_text(fence_content) + "</PRE>\n";
                fence_content.clear();
            } else {
                fence_content += line + "\n";
            }
        }
    }
    // unclosed fence — emit what we have rather than silently dropping it
    if (in_fence && !fence_content.empty()) {
        result += "<PRE>" + encode_html_text(fence_content) + "</PRE>\n";
    }
    return result;
}

// Replaces \n with <BR> only *outside* <PRE> blocks.
// Inside PRE the browser already preserves newlines as line-breaks; adding <BR>
// there too would cause double spacing on every code line.
static std::string newlines_to_br_outside_pre(const std::string& s) {
    std::string result;
    result.reserve(s.size() + 64);
    bool in_pre = false;
    size_t i = 0;

    // Case-insensitive substring match starting at position i
    auto ci_match = [&](const char* needle, size_t len) -> bool {
        if (i + len > s.size()) return false;
        for (size_t j = 0; j < len; j++) {
            if (std::tolower(static_cast<unsigned char>(s[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) return false;
        }
        return true;
    };

    while (i < s.size()) {
        // Detect opening <pre> or <PRE> — require > or whitespace immediately after
        // to avoid false positives on words like <predefined>
        if (!in_pre && ci_match("<pre", 4) &&
            i + 4 < s.size() && (s[i+4] == '>' || s[i+4] == ' ' || s[i+4] == '\t')) {
            in_pre = true;
            result += s[i++];
            continue;
        }
        // Detect closing </pre>
        if (in_pre && ci_match("</pre>", 6)) {
            in_pre = false;
            result.append(s, i, 6);
            i += 6;
            continue;
        }
        // Only convert newlines to <BR> when we're outside a PRE block
        if (!in_pre && s[i] == '\n') {
            result += "<BR>\n";
            i++;
            continue;
        }
        result += s[i++];
    }
    return result;
}

// =============================================================================
// Chat page — Yahoo! Chat room aesthetic, single-turn conversation
// =============================================================================

std::string render_chat_page(const std::string& msg,
                              const std::string& response,
                              const std::string& persona_name) {
    std::ostringstream html;
    html << R"html(<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 3.2//EN">
<HTML>
<HEAD>
<TITLE>Boohoo! Chat - The Boohoo! Lounge</TITLE>
</HEAD>
<BODY BGCOLOR="#000033" TEXT="#FFFFFF" LINK="#FFFF00" VLINK="#FF88FF">
<CENTER>
<TABLE WIDTH="620" BGCOLOR="#000099" BORDER="0" CELLPADDING="6" CELLSPACING="0">
<TR>
<TD>
<FONT COLOR="#FFFF00" SIZE="5"><B>Boohoo! Chat</B></FONT>
&nbsp;&mdash;&nbsp;
<FONT COLOR="#AAAAFF" SIZE="3">The Boohoo! Lounge</FONT>
&nbsp;&nbsp;&nbsp;
<FONT COLOR="#888888" SIZE="2"><I>42 users chatting now</I></FONT>
</TD>
</TR>
</TABLE>
<MARQUEE BGCOLOR="#000066" WIDTH="620">
<FONT COLOR="#00FFFF" SIZE="2">&nbsp;&nbsp;&nbsp;Welcome to The Boohoo! Lounge!
&nbsp;&#9830;&nbsp; The premier chat destination on the World Wide Web!
&nbsp;&#9830;&nbsp; Be nice or be gone!
&nbsp;&#9830;&nbsp; A/S/L?
&nbsp;&nbsp;&nbsp;</FONT>
</MARQUEE>
<TABLE WIDTH="620" BORDER="2" CELLPADDING="8" BGCOLOR="#000022" BORDERCOLOR="#0000AA">
<TR>
<TD>
)html";

    if (msg.empty()) {
        html << R"html(<FONT COLOR="#888888" SIZE="2"><I>
[The Boohoo! Lounge] Welcome! Type a message below to begin chatting.<BR>
h4x0r_1337 is lurking and ready to drop knowledge.
</I></FONT>)html";
    } else {
        // User message: always fully encoded — it's external input.
        std::string safe_msg  = encode_html_text(msg);
        std::string safe_name = encode_html_text(persona_name);

        // Process the model response through three stages:
        //
        // Stage 1: ``` fences → <PRE> blocks with HTML-encoded content.
        //   Models emit markdown even when asked for HTML; we intercept here.
        //   encode_html_text() inside the converter handles <stdio.h> etc.
        //
        // Stage 2: fix_code_blocks() catches any <pre> tags the model wrote
        //   directly (belt-and-suspenders in case stage 1 didn't fire).
        //
        // Stage 3: convert remaining \n to <BR> *outside* PRE blocks only.
        //   Inside PRE, the browser preserves newlines natively; adding <BR>
        //   there too would double-space every code line.
        std::string processed   = convert_markdown_code_fences(response);
        processed               = fix_code_blocks(processed);
        std::string display_resp = newlines_to_br_outside_pre(processed);

        html << R"html(<TABLE WIDTH="100%" BORDER="0" CELLPADDING="4">
<TR VALIGN="TOP">
<TD>
<FONT COLOR="#FFFF00"><B>[Guest_1337]:</B></FONT>
&nbsp;
<FONT COLOR="#FFFFFF">)html" << safe_msg << R"html(</FONT>
</TD>
</TR>
<TR VALIGN="TOP">
<TD BGCOLOR="#001133">
<FONT COLOR="#00FF88"><B>[)html" << safe_name << R"html(]:</B></FONT><BR>
<FONT COLOR="#CCFFEE">)html" << display_resp << R"html(</FONT>
</TD>
</TR>
</TABLE>)html";
    }

    html << R"html(
</TD>
</TR>
</TABLE>
<TABLE WIDTH="620" BGCOLOR="#000066" BORDER="0" CELLPADDING="6" CELLSPACING="0">
<TR>
<TD>
<FORM METHOD="GET" ACTION="/chat">
<FONT COLOR="#AAAAFF" SIZE="2"><B>Your message:</B></FONT>
&nbsp;&nbsp;
<INPUT TYPE="TEXT" NAME="msg" SIZE="46" MAXLENGTH="500">
&nbsp;
<INPUT TYPE="SUBMIT" VALUE="Say it!">
</FORM>
</TD>
</TR>
</TABLE>
<HR WIDTH="620" SIZE="1" COLOR="#333399">
<FONT SIZE="1" COLOR="#666688">
<A HREF="/">[Back to Boohoo!]</A>
&nbsp;&nbsp;|&nbsp;&nbsp;
Boohoo! Chat &copy; 1997 Boohoo! Inc.
&nbsp;&nbsp;|&nbsp;&nbsp;
Best viewed in Netscape Navigator 3.0 at 800x600
&nbsp;&nbsp;|&nbsp;&nbsp;
<I>Now playing: cyberchat.mid</I>
</FONT>
</CENTER>
</BODY>
</HTML>)html";

    return html.str();
}

// =============================================================================
// Home page — hardcoded Yahoo! 1996 aesthetic
// =============================================================================

std::string render_home() {
    return R"html(<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 3.2//EN">
<HTML>
<HEAD>
<TITLE>Boohoo!</TITLE>
</HEAD>
<BODY BGCOLOR="#FFFFFF" TEXT="#000000" LINK="#0000FF" VLINK="#551A8B" ALINK="#FF0000">
<CENTER>
<TABLE WIDTH="600" BORDER="0" CELLPADDING="4" CELLSPACING="0">
<TR>
<TD ALIGN="CENTER">
<FONT COLOR="#FF0000" SIZE="7"><B>B</B></FONT><FONT COLOR="#800080" SIZE="7"><B>oohoo!</B></FONT>
<P><FONT SIZE="2" COLOR="#666666"><I>A guide to the World Wide Web</I></FONT></P>
<FORM METHOD="GET" ACTION="/search">
<INPUT TYPE="TEXT" NAME="q" SIZE="40" MAXLENGTH="200">
&nbsp;
<INPUT TYPE="SUBMIT" VALUE="Search">
</FORM>
<HR WIDTH="90%" SIZE="1" COLOR="#CCCCCC">
</TD>
</TR>
<TR>
<TD BGCOLOR="#000080" ALIGN="CENTER">
<FONT COLOR="#FFFF00" SIZE="2"><B>&#9670; NEW! &#9670;</B></FONT>
&nbsp;&nbsp;
<A HREF="/chat"><FONT COLOR="#FFFFFF" SIZE="3"><B>Boohoo! Chat Rooms</B></FONT></A>
&nbsp;&nbsp;
<FONT COLOR="#CCCCFF" SIZE="2"><I>Chat live with the locals!</I></FONT>
</TD>
</TR>
<TR>
<TD>
<TABLE WIDTH="100%" BORDER="0" CELLPADDING="6">
<TR VALIGN="TOP">
<TD WIDTH="50%">
<FONT SIZE="3"><B><A HREF="/search?q=arts+and+humanities">Arts &amp; Humanities</A></B></FONT><BR>
<FONT SIZE="2" COLOR="#666666">Literature, Photography, Architecture...</FONT>
<P>
<FONT SIZE="3"><B><A HREF="/search?q=business+and+economy">Business &amp; Economy</A></B></FONT><BR>
<FONT SIZE="2" COLOR="#666666">Companies, Finance, Jobs, Investing...</FONT>
<P>
<FONT SIZE="3"><B><A HREF="/search?q=computers+and+internet">Computers &amp; Internet</A></B></FONT><BR>
<FONT SIZE="2" COLOR="#666666">Internet, WWW, Software, Hardware...</FONT>
<P>
<FONT SIZE="3"><B><A HREF="/search?q=education">Education</A></B></FONT><BR>
<FONT SIZE="2" COLOR="#666666">Universities, K-12, Distance Learning...</FONT>
<P>
<FONT SIZE="3"><B><A HREF="/search?q=entertainment">Entertainment</A></B></FONT><BR>
<FONT SIZE="2" COLOR="#666666">Movies, Music, Television, Games...</FONT>
</TD>
<TD WIDTH="50%">
<FONT SIZE="3"><B><A HREF="/search?q=health">Health</A></B></FONT><BR>
<FONT SIZE="2" COLOR="#666666">Medicine, Fitness, Nutrition...</FONT>
<P>
<FONT SIZE="3"><B><A HREF="/search?q=news+and+media">News &amp; Media</A></B></FONT><BR>
<FONT SIZE="2" COLOR="#666666">Current Events, Newspapers, TV...</FONT>
<P>
<FONT SIZE="3"><B><A HREF="/search?q=recreation+and+sports">Recreation &amp; Sports</A></B></FONT><BR>
<FONT SIZE="2" COLOR="#666666">Sports, Hobbies, Travel, Outdoors...</FONT>
<P>
<FONT SIZE="3"><B><A HREF="/search?q=science">Science</A></B></FONT><BR>
<FONT SIZE="2" COLOR="#666666">Animals, Astronomy, Biology...</FONT>
<P>
<FONT SIZE="3"><B><A HREF="/search?q=society+and+culture">Society &amp; Culture</A></B></FONT><BR>
<FONT SIZE="2" COLOR="#666666">History, People, Religion...</FONT>
</TD>
</TR>
</TABLE>
</TD>
</TR>
</TABLE>
<HR WIDTH="600" SIZE="1" COLOR="#CCCCCC">
<FONT SIZE="1" COLOR="#666666">
Copyright &copy; 1997 Boohoo! Inc. &nbsp;|&nbsp;
<A HREF="/search?q=about+boohoo">About Boohoo!</A> &nbsp;|&nbsp;
<A HREF="/search?q=help">Help</A>
</FONT>
</CENTER>
</BODY>
</HTML>)html";
}

// =============================================================================
// Search results — parse model output, render Boohoo!-style results page
// =============================================================================

struct SearchResult {
    std::string title;
    std::string url;
    std::string category;
    std::string desc;
};

// Parse the structured model output into SearchResult objects.
// Expected format (repeated, separated by ---):
//   TITLE: Some Title
//   URL: /some/path
//   CATEGORY: Category > Sub
//   DESC: Description text.
// Returns true for separator lines: "---", "===", or any run of those chars
static bool is_separator(const std::string& l) {
    if (l.empty()) return false;
    return l.find_first_not_of("-=*~ \t") == std::string::npos && l.size() >= 2;
}

static void fill_blanks(SearchResult& r) {
    if (r.url.empty())      r.url      = "/~unknown/page.html";
    if (r.category.empty()) r.category = "Miscellaneous";
    if (r.desc.empty())     r.desc     = "(No description available)";
}

// Primary parser: expects labeled fields (TITLE: / URL: / CATEGORY: / DESC:).
// Returns results, which may be empty if the model didn't use labels.
static std::vector<SearchResult> parse_labeled(const std::string& raw) {
    std::vector<SearchResult> results;
    std::istringstream stream(raw);
    std::string line;
    SearchResult current;

    auto flush = [&]() {
        if (!current.title.empty()) {
            fill_blanks(current);
            results.push_back(current);
            current = SearchResult{};
        }
    };

    while (std::getline(stream, line)) {
        line = trim(line);
        if (is_separator(line)) { flush(); continue; }

        std::string val;
        if (!(val = extract_field(line, "TITLE")).empty()) {
            if (!current.title.empty()) flush();  // model skipped the separator
            current.title = val;
        } else if (!(val = extract_field(line, "URL")).empty()) {
            current.url = val;
        } else if (!(val = extract_field(line, "CATEGORY")).empty()) {
            current.category = val;
        } else if (!(val = extract_field(line, "DESC")).empty()) {
            current.desc = val;
        }
    }
    flush();
    return results;
}

// Fallback parser: handles bare format where the model skipped field labels.
// Splits on "---" then classifies each line by content:
//   URL       — starts with http:// or https:// or /
//   Category  — contains " > " (Yahoo-style breadcrumb)
//   Title     — first plain text line in the block
//   Desc      — remaining plain text
static std::vector<SearchResult> parse_bare(const std::string& raw) {
    std::vector<SearchResult> results;

    // Split the whole output into blocks on separator lines
    std::vector<std::string> blocks;
    std::string block;
    std::istringstream stream(raw);
    std::string line;
    while (std::getline(stream, line)) {
        if (is_separator(trim(line))) {
            if (!trim(block).empty()) blocks.push_back(block);
            block.clear();
        } else {
            block += line + "\n";
        }
    }
    if (!trim(block).empty()) blocks.push_back(block);

    for (const auto& b : blocks) {
        SearchResult r;
        std::istringstream bs(b);
        std::string l;
        while (std::getline(bs, l)) {
            l = trim(l);
            if (l.empty()) continue;

            // URL: starts with http(s):// or a bare /path
            if ((l.size() > 7 && l.substr(0, 7) == "http://") ||
                (l.size() > 8 && l.substr(0, 8) == "https://") ||
                (!l.empty() && l[0] == '/')) {
                if (r.url.empty()) r.url = l;
                continue;
            }

            // Category: contains " > " which is the Yahoo breadcrumb style
            if (l.find(" > ") != std::string::npos) {
                if (r.category.empty()) r.category = l;
                continue;
            }

            // First remaining line = title, subsequent ones = description
            if (r.title.empty()) {
                r.title = l;
            } else if (r.desc.empty()) {
                r.desc = l;
            }
        }

        if (!r.title.empty()) {
            fill_blanks(r);
            results.push_back(r);
        }
    }
    return results;
}

// Try labeled format first; fall back to bare format if that yields nothing.
static std::vector<SearchResult> parse_results(const std::string& raw) {
    auto results = parse_labeled(raw);
    if (results.empty()) results = parse_bare(raw);
    return results;
}

std::string render_search_results(const std::string& query,
                                   const std::string& raw_output) {
    auto results = parse_results(raw_output);

    // HTML-encode the query before inserting it into any HTML context.
    // Without this, a query like "><script>... would break out of attribute
    // values or tag bodies and execute in any modern browser hitting the server.
    const std::string safe_query = encode_html_text(query);

    std::ostringstream html;
    html << R"html(<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 3.2//EN">
<HTML>
<HEAD>
<TITLE>Boohoo! Search Results: )html" << safe_query << R"html(</TITLE>
</HEAD>
<BODY BGCOLOR="#FFFFFF" TEXT="#000000" LINK="#0000FF" VLINK="#551A8B">
<TABLE WIDTH="600" BORDER="0" CELLPADDING="4">
<TR>
<TD>
<FONT COLOR="#FF0000" SIZE="5"><B>B</B></FONT><FONT COLOR="#800080" SIZE="5"><B>oohoo!</B></FONT>
&nbsp;&nbsp;
<FORM METHOD="GET" ACTION="/search" STYLE="display:inline">
<INPUT TYPE="TEXT" NAME="q" VALUE=")html" << safe_query << R"html(" SIZE="30">
<INPUT TYPE="SUBMIT" VALUE="Search Again">
</FORM>
</TD>
</TR>
</TABLE>
<HR WIDTH="600" SIZE="1" COLOR="#CCCCCC">
)html";

    if (results.empty()) {
        html << R"html(<TABLE WIDTH="600" BORDER="0" CELLPADDING="8">
<TR><TD>
<FONT SIZE="3">No documents match the query <B>)html" << safe_query << R"html(</B>.</FONT><BR>
<FONT SIZE="2" COLOR="#666666">Try different keywords or check spelling.</FONT>
</TD></TR>
</TABLE>
)html";
    } else {
        html << R"html(<TABLE WIDTH="600" BORDER="0" CELLPADDING="2">
<TR><TD>
<FONT SIZE="2" COLOR="#666666">
Found )html" << results.size() << R"html( sites matching <B>)html" << safe_query << R"html(</B>
</FONT>
</TD></TR>
</TABLE>
<P>
)html";

        for (const auto& r : results) {
            // The /page route accepts a topic parameter built from the result title.
            // url_encode handles spaces and special characters.
            // Pass both the result title and the original search query so the
            // page generator has full context — a title like "The Analog Guru"
            // is much more useful when we also know it came from "best analog synthesizers".
            std::string page_url = "/page?topic=" + url_encode(r.title)
                                 + "&q=" + url_encode(query);

            html << R"html(<TABLE WIDTH="600" BORDER="0" CELLPADDING="4" CELLSPACING="0">
<TR VALIGN="TOP">
<TD WIDTH="16" ALIGN="CENTER"><FONT SIZE="4" COLOR="#FF8800">&#8226;</FONT></TD>
<TD>
<FONT SIZE="4"><B><A HREF=")html" << page_url << R"html(">)html" << r.title << R"html(</A></B></FONT><BR>
<FONT SIZE="2" COLOR="#008800">)html" << r.category << R"html(</FONT><BR>
<FONT SIZE="3">)html" << r.desc << R"html(</FONT><BR>
<FONT SIZE="2" COLOR="#008000"><I>)html" << r.url << R"html(</I></FONT>
</TD>
</TR>
</TABLE>
<HR WIDTH="600" SIZE="1" COLOR="#EEEEEE">
)html";
        }
    }

    html << R"html(<P>
<TABLE WIDTH="600" BORDER="0" CELLPADDING="4">
<TR>
<TD ALIGN="CENTER">
<A HREF="/">[Back to Boohoo!]</A>
&nbsp;&nbsp;
<FONT SIZE="1" COLOR="#666666">Copyright &copy; 1997 Boohoo! Inc.</FONT>
</TD>
</TR>
</TABLE>
</BODY>
</HTML>)html";

    return html.str();
}

// =============================================================================
// Content page — post-process model output
// =============================================================================

std::string render_content_page(const std::string& raw_html) {
    // Step 1: remove anything before the opening <HTML tag
    std::string html = strip_preamble(raw_html);

    // Step 2: encode < > & inside <PRE> blocks so C/HTML code renders correctly
    // instead of being swallowed by the browser as tags.
    html = fix_code_blocks(html);

    // Step 3: if BODY BGCOLOR and TEXT colours are too similar to read, replace
    // TEXT with whichever of black or white gives better contrast.
    html = fix_color_contrast(html);

    // Step 4: replace !!false fact!! markers with red italic text.
    html = highlight_false_facts(html);

    return html;
}
