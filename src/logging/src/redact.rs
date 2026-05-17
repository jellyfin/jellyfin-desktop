//! Token redaction for log output. Detects known query-param / JSON / header
//! patterns that precede a Jellyfin access token and overwrites the token
//! value with 'x' characters in place, preserving URL/JSON shape.

struct PatternRule {
    needle: &'static [u8],
    terminators: &'static [u8],
}

const URL_TERMINATORS: &[u8] = b"&\"' \t\r\n;<>";
const JSON_TERMINATORS: &[u8] = b"\"";

const RULES: &[PatternRule] = &[
    PatternRule {
        needle: b"api_key=",
        terminators: URL_TERMINATORS,
    },
    PatternRule {
        needle: b"X-MediaBrowser-Token%3D",
        terminators: URL_TERMINATORS,
    },
    PatternRule {
        needle: b"X-MediaBrowser-Token=",
        terminators: URL_TERMINATORS,
    },
    PatternRule {
        needle: b"ApiKey=",
        terminators: URL_TERMINATORS,
    },
    PatternRule {
        needle: b"AccessToken=",
        terminators: URL_TERMINATORS,
    },
    PatternRule {
        needle: b"AccessToken\":\"",
        terminators: JSON_TERMINATORS,
    },
];

fn find_subslice(haystack: &[u8], needle: &[u8], from: usize) -> Option<usize> {
    if needle.is_empty() || from > haystack.len() {
        return None;
    }
    haystack[from..]
        .windows(needle.len())
        .position(|w| w == needle)
        .map(|p| p + from)
}

fn find_token_end(buf: &[u8], from: usize, terminators: &[u8]) -> usize {
    buf[from..]
        .iter()
        .position(|c| terminators.contains(c))
        .map(|p| p + from)
        .unwrap_or(buf.len())
}

fn elide(buf: &mut [u8], rule: &PatternRule) {
    let mut start = 0;
    while let Some(pos) = find_subslice(buf, rule.needle, start) {
        let token_start = pos + rule.needle.len();
        let token_end = find_token_end(buf, token_start, rule.terminators);
        for b in &mut buf[token_start..token_end] {
            *b = b'x';
        }
        start = if token_end > token_start {
            token_end
        } else {
            token_start
        };
    }
}

pub fn contains_secret(buf: &[u8]) -> bool {
    for rule in RULES {
        if let Some(pos) = find_subslice(buf, rule.needle, 0) {
            let token_start = pos + rule.needle.len();
            if token_start < buf.len() && !rule.terminators.contains(&buf[token_start]) {
                return true;
            }
        }
    }
    false
}

pub fn censor(buf: &mut [u8]) {
    for rule in RULES {
        elide(buf, rule);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn censor_str(s: &str) -> String {
        let mut bytes = s.as_bytes().to_vec();
        censor(&mut bytes);
        String::from_utf8(bytes).unwrap()
    }

    #[test]
    fn url_token() {
        assert_eq!(
            censor_str("/path?api_key=abc123&x=1"),
            "/path?api_key=xxxxxx&x=1"
        );
        assert!(contains_secret(b"/path?api_key=abc"));
    }

    #[test]
    fn json_token() {
        assert_eq!(
            censor_str("\"AccessToken\":\"abc\""),
            "\"AccessToken\":\"xxx\""
        );
    }

    #[test]
    fn empty_token() {
        assert_eq!(censor_str("api_key=&x=1"), "api_key=&x=1");
        assert!(!contains_secret(b"api_key=&x=1"));
    }

    #[test]
    fn header_encoded() {
        assert_eq!(
            censor_str("X-MediaBrowser-Token%3Dabcdef HTTP"),
            "X-MediaBrowser-Token%3Dxxxxxx HTTP"
        );
    }

    #[test]
    fn no_pattern() {
        assert_eq!(censor_str("plain message"), "plain message");
        assert!(!contains_secret(b"plain message"));
    }
}
