
use std::collections::HashMap;

pub fn load(path: &str) -> HashMap<String, String> {
    let mut map = HashMap::new();
    let Ok(content) = std::fs::read_to_string(path) else {
        tracing::warn!("could not read OUI file {}", path);
        return map;
    };
    for line in content.lines().skip(1) {
        let parts: Vec<&str> = line.splitn(3, ',').collect();
        if parts.len() >= 2 {
            let prefix = parts[0].trim().to_lowercase().replace(':', "").replace('-', "");
            let vendor = parts[1].trim().trim_matches('"').to_string();
            if prefix.len() >= 6 {
                map.insert(prefix[..6].to_string(), vendor);
            }
        }
    }
    map
}

pub fn lookup(db: &HashMap<String, String>, mac: &str) -> String {
    let clean: String = mac.chars().filter(|c| c.is_ascii_hexdigit()).collect();
    if clean.len() >= 6 {
        db.get(&clean[..6].to_lowercase())
            .cloned()
            .unwrap_or_else(|| "Unknown".to_string())
    } else {
        "Unknown".to_string()
    }
}
