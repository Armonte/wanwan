// host_library.h -- transforms between the games TSV and what the UI shows.
//
// Split out of host.cpp at the repo's 1000-line ceiling. These are pure
// functions over strings: they take the published library and produce the
// rail's chips, the search-filtered list, the live online counts, and the
// title and stem columns the host indexes by. None of them touch the UI or
// the hub; the caller owns all of that.
#pragma once
#include <functional>

#include <string>
#include <vector>

namespace hostgui {

// Split a title across a rail chip's two lines, breaking on spaces so a
// word is never cut in half, and measuring DISPLAY COLUMNS rather than
// bytes -- a full-width glyph occupies two columns of a line that fits
// four Latin characters.
void RailName(const std::string& in, std::string* line1, std::string* line2);

// Field 0 of every published row, in order.
std::vector<std::string> RowTitles(const std::string& tsv);

// Field 7 of every published row -- the exe stem, which is the id the hub
// keys rooms and replays by.
std::vector<std::string> RowStems(const std::string& tsv);
std::vector<std::string> RowPaths(const std::string& tsv);    // field 8, the exe
std::vector<std::string> RowEngines(const std::string& tsv);  // field 3, FM2K / FM95

// Narrow a library TSV to rows whose title contains `needle`, matched
// case-insensitively. An empty needle returns everything.
std::string FilterByTitle(const std::string& tsv, const std::string& needle);

// The rail's chip list: MRU order first, then anything else to fill the
// slots, as "index\tline1\tline2" per line.
// `unread(index)` answers "how many lines has this game's room had since
// you last looked at it" -- the rail is the only place that shows it, so
// the count is fetched per row rather than passed in as a parallel array.
std::string RailRows(const std::vector<std::string>& titles,
                     const std::vector<std::string>& mru,
                     int max_chips,
                     const std::function<int(size_t)>& unread = {});

// The browse body as grid rows: "head\textra\tdim\ti0..i4" per line.
//
// Sections are built HERE because each one is a question about data the
// host holds and the UI does not: who is online right now, what you played
// recently, what nobody is playing. A search collapses them into a single
// results list, because sectioning a filtered list tells you nothing.
// How many 320px tiles fit a row; the host sets it from the window width.
void SetBrowseColumns(int cols);
int  BrowseColumns();
std::string BrowseRows(const std::string& tsv,
                       const std::vector<std::string>& mru,
                       bool searching);

}  // namespace hostgui
