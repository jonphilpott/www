#pragma once
#include <string>

// Yahoo!-style home page with search form
std::string render_home();

// Yahoo!-style search results page.
// raw_output should contain TITLE:/URL:/CATEGORY:/DESC: blocks separated by "---"
std::string render_search_results(const std::string& query,
                                   const std::string& raw_output);

// Post-processes a model-generated content page:
//   - strips any preamble before <HTML
//   - replaces !!false fact!! markers with red italic HTML
std::string render_content_page(const std::string& raw_html);

// Boohoo! Chat room page.
// msg and response are empty strings when the user hasn't sent anything yet.
// persona_name is the display name of the randomly selected persona.
std::string render_chat_page(const std::string& msg,
                              const std::string& response,
                              const std::string& persona_name);

// URL-encodes a string for use in query parameters (spaces become +, etc.)
std::string url_encode(const std::string& s);
