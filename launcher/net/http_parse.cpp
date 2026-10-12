// HTTP/1.1 client-side parsing. See http_parse.h.
#include "http_parse.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace fm2k::net {

namespace {

char Lower(char c) { return (char)std::tolower((unsigned char)c); }

std::string LowerStr(std::string s) {
    for (char& c : s) c = Lower(c);
    return s;
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

uint16_t DefaultPort(const std::string& scheme) {
    return (scheme == "https" || scheme == "wss") ? 443 : 80;
}

// Parses a decimal port; false on empty, non-digit, 0 or > 65535.
bool ParsePort(const std::string& s, uint16_t& out) {
    if (s.empty() || s.size() > 5) return false;
    unsigned v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (unsigned)(c - '0');
    }
    if (v == 0 || v > 65535) return false;
    out = (uint16_t)v;
    return true;
}

}  // namespace

bool IEquals(const std::string& a, const char* b) {
    const size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (Lower(a[i]) != Lower(b[i])) return false;
    }
    return true;
}

const std::string* FindHeader(const std::vector<Header>& headers, const char* name) {
    for (const auto& h : headers) {
        if (IEquals(h.name, name)) return &h.value;
    }
    return nullptr;
}

bool HeaderHasToken(const std::string& value, const char* token) {
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        if (comma == std::string::npos) comma = value.size();
        if (IEquals(Trim(value.substr(start, comma - start)), token)) return true;
        start = comma + 1;
    }
    return false;
}

bool ParseUrl(const std::string& url, Url& out) {
    out = Url{};
    const size_t sep = url.find("://");
    if (sep == std::string::npos || sep == 0) return false;
    out.scheme = LowerStr(url.substr(0, sep));
    if (out.scheme != "http" && out.scheme != "https" &&
        out.scheme != "ws" && out.scheme != "wss") {
        return false;
    }
    out.tls = (out.scheme == "https" || out.scheme == "wss");

    const size_t auth_start = sep + 3;
    size_t auth_end = url.find_first_of("/?#", auth_start);
    if (auth_end == std::string::npos) auth_end = url.size();
    const std::string authority = url.substr(auth_start, auth_end - auth_start);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;

    std::string host, port_str;
    if (authority[0] == '[') {                       // IPv6 literal
        const size_t close = authority.find(']');
        if (close == std::string::npos || close == 1) return false;
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return false;
            port_str = authority.substr(close + 2);
            if (port_str.empty()) return false;
        }
    } else {
        const size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            host     = authority.substr(0, colon);
            port_str = authority.substr(colon + 1);
            if (port_str.empty()) return false;
        } else {
            host = authority;
        }
    }
    if (host.empty()) return false;
    for (char c : host) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return false;
    }
    out.host = LowerStr(host);
    out.port = DefaultPort(out.scheme);
    if (!port_str.empty()) {
        if (!ParsePort(port_str, out.port)) return false;
        out.explicit_port = true;
    }

    std::string rest = url.substr(auth_end);
    const size_t hash = rest.find('#');          // fragments never go on the wire
    if (hash != std::string::npos) rest.resize(hash);
    if (rest.empty() || rest[0] == '?') rest.insert(rest.begin(), '/');
    for (char c : rest) {
        if (c == ' ' || c == '\r' || c == '\n') return false;
    }
    out.target = rest;
    return true;
}

std::string HostHeader(const Url& u) {
    std::string h = (u.host.find(':') != std::string::npos) ? "[" + u.host + "]" : u.host;
    if (u.port != DefaultPort(u.scheme)) h += ":" + std::to_string(u.port);
    return h;
}

bool ResolveRedirect(const Url& base, const std::string& location, Url& out) {
    const std::string loc = Trim(location);
    if (loc.empty()) return false;
    if (loc.find("://") != std::string::npos) {
        if (!ParseUrl(loc, out)) return false;
    } else if (loc.size() >= 2 && loc[0] == '/' && loc[1] == '/') {
        if (!ParseUrl(base.scheme + ":" + loc, out)) return false;
    } else {
        out = base;
        std::string target;
        if (loc[0] == '/') {
            target = loc;
        } else if (loc[0] == '?') {
            const size_t q = base.target.find('?');
            target = base.target.substr(0, q) + loc;
        } else {
            std::string dir = base.target.substr(0, base.target.find('?'));
            dir = dir.substr(0, dir.rfind('/') + 1);
            target = dir + loc;
        }
        const size_t hash = target.find('#');
        if (hash != std::string::npos) target.resize(hash);
        out.target = target;
    }
    return out.scheme == "http" || out.scheme == "https";
}

long ParseResponseHead(const char* data, size_t len, ResponseHead& out) {
    // Find the end of the head: CRLFCRLF, or LFLF for lenient servers.
    size_t end = std::string::npos, term = 0;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] != '\n') continue;
        if (i + 1 < len && data[i + 1] == '\n') { end = i; term = 2; break; }
        if (i + 2 < len && data[i + 1] == '\r' && data[i + 2] == '\n') {
            end = i; term = 3; break;
        }
    }
    if (end == std::string::npos) {
        return (len > kMaxResponseHead) ? -1 : 0;
    }
    if (end + term > kMaxResponseHead) return -1;

    out = ResponseHead{};
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= end) {
        const void* nl = std::memchr(data + pos, '\n', end + 1 - pos);
        size_t line_end = nl ? (size_t)((const char*)nl - data) : end + 1;
        size_t trim_end = line_end;
        if (trim_end > pos && data[trim_end - 1] == '\r') --trim_end;
        lines.emplace_back(data + pos, trim_end - pos);
        pos = line_end + 1;
    }
    if (lines.empty()) return -1;

    // Status line: HTTP/1.x SP 3DIGIT [SP reason]
    const std::string& sl = lines[0];
    if (sl.size() < 12 || sl.compare(0, 7, "HTTP/1.") != 0 || sl[8] != ' ') return -1;
    if (sl[7] < '0' || sl[7] > '9') return -1;
    out.minor_version = sl[7] - '0';
    for (int i = 9; i < 12; ++i) {
        if (sl[i] < '0' || sl[i] > '9') return -1;
    }
    out.status = (sl[9] - '0') * 100 + (sl[10] - '0') * 10 + (sl[11] - '0');
    if (out.status < 100) return -1;
    if (sl.size() > 12) {
        if (sl[12] != ' ') return -1;
        out.reason = sl.substr(13);
    }

    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        if (l.empty()) continue;
        if ((l[0] == ' ' || l[0] == '\t') && !out.headers.empty()) {
            out.headers.back().value += " " + Trim(l);   // obsolete line folding
            continue;
        }
        const size_t colon = l.find(':');
        if (colon == std::string::npos || colon == 0) return -1;
        Header h;
        h.name  = l.substr(0, colon);
        if (h.name.find_first_of(" \t") != std::string::npos) return -1;
        h.value = Trim(l.substr(colon + 1));
        out.headers.push_back(std::move(h));
    }
    return (long)(end + term);
}

bool BodyDecoder::Init(const ResponseHead& head, bool head_request) {
    *this = BodyDecoder{};
    if (head_request || head.status < 200 || head.status == 204 || head.status == 304) {
        mode_ = Mode::None;
        done_ = true;
        return true;
    }
    const std::string* te = FindHeader(head.headers, "Transfer-Encoding");
    if (te && HeaderHasToken(*te, "chunked")) {
        mode_ = Mode::Chunked;
        return true;
    }
    const std::string* cl = FindHeader(head.headers, "Content-Length");
    if (cl) {
        if (cl->empty() || cl->size() > 18) { error_ = true; return false; }
        uint64_t v = 0;
        for (char c : *cl) {
            if (c < '0' || c > '9') { error_ = true; return false; }
            v = v * 10 + (uint64_t)(c - '0');
        }
        mode_      = Mode::ContentLength;
        total_     = v;
        remaining_ = v;
        done_      = (v == 0);
        return true;
    }
    mode_ = Mode::UntilClose;
    return true;
}

long BodyDecoder::Feed(const char* data, size_t len, std::string& out) {
    if (error_) return -1;
    if (done_ || len == 0) return 0;

    if (mode_ == Mode::ContentLength) {
        const size_t take = (size_t)((uint64_t)len < remaining_ ? (uint64_t)len : remaining_);
        out.append(data, take);
        remaining_ -= take;
        if (remaining_ == 0) done_ = true;
        return (long)take;
    }
    if (mode_ == Mode::UntilClose) {
        out.append(data, len);
        return (long)len;
    }

    // Chunked.
    size_t i = 0;
    auto fail = [this]() { error_ = true; return -1L; };
    auto end_size_line = [this]() {
        if (size_digits_ == 0) return false;
        if (chunk_size_ == 0) {
            cstate_ = Chunk::Trailer;
            trailer_line_empty_ = true;
        } else {
            cstate_    = Chunk::Data;
            remaining_ = chunk_size_;
        }
        return true;
    };
    while (i < len && !done_) {
        const char c = data[i];
        switch (cstate_) {
        case Chunk::Size: {
            int v = -1;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            if (v >= 0) {
                if (++size_digits_ > 15) return fail();
                chunk_size_ = chunk_size_ * 16 + (uint64_t)v;
            } else if (c == ';' || c == ' ' || c == '\t') {
                cstate_ = Chunk::Ext;
            } else if (c == '\r') {
                cstate_ = Chunk::SizeLf;
            } else if (c == '\n') {
                if (!end_size_line()) return fail();
            } else {
                return fail();
            }
            ++i;
            break;
        }
        case Chunk::Ext:
            if (c == '\r') cstate_ = Chunk::SizeLf;
            else if (c == '\n' && !end_size_line()) return fail();
            ++i;
            break;
        case Chunk::SizeLf:
            if (c != '\n' || !end_size_line()) return fail();
            ++i;
            break;
        case Chunk::Data: {
            const size_t avail = len - i;
            const size_t take = (size_t)((uint64_t)avail < remaining_ ? (uint64_t)avail : remaining_);
            out.append(data + i, take);
            remaining_ -= take;
            i += take;
            if (remaining_ == 0) cstate_ = Chunk::DataCr;
            break;
        }
        case Chunk::DataCr:
            if (c == '\r') cstate_ = Chunk::DataLf;
            else if (c == '\n') { cstate_ = Chunk::Size; chunk_size_ = 0; size_digits_ = 0; }
            else return fail();
            ++i;
            break;
        case Chunk::DataLf:
            if (c != '\n') return fail();
            cstate_ = Chunk::Size; chunk_size_ = 0; size_digits_ = 0;
            ++i;
            break;
        case Chunk::Trailer:
            if (c == '\r') {
                cstate_ = Chunk::TrailerLf;
            } else if (c == '\n') {
                if (trailer_line_empty_) done_ = true;
                trailer_line_empty_ = true;
            } else {
                trailer_line_empty_ = false;
            }
            ++i;
            break;
        case Chunk::TrailerLf:
            if (c != '\n') return fail();
            if (trailer_line_empty_) done_ = true;
            trailer_line_empty_ = true;
            cstate_ = Chunk::Trailer;
            ++i;
            break;
        }
    }
    return (long)i;
}

bool BodyDecoder::OnEof() {
    if (done_) return true;
    if (mode_ == Mode::UntilClose) { done_ = true; return true; }
    return false;
}

}  // namespace fm2k::net
