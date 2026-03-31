
use reqwest::Client;
use serde_json::json;

pub async fn analyze(summary: String) -> String {
    let api_key = match std::env::var("OPENROUTER_API_KEY") {
        Ok(k) if !k.is_empty() && k != "your-key-here" => k,
        _ => { tracing::warn!("OPENROUTER_API_KEY not set"); return String::new(); }
    };

    let model = std::env::var("LLM_MODEL")
        .unwrap_or("deepseek/deepseek-chat-v3-0324:free".into());

    let prompt = format!(
        "You are a WiFi network security analyst. Analyze this 30-second packet capture summary and write 3-4 sentences of plain-English analysis. Flag anything suspicious. Lead with the most important observation. Be direct. No bullet points.\n\n{}",
        summary
    );

    let client = Client::new();
    let res = client
        .post("https://openrouter.ai/api/v1/chat/completions")
        .header("Authorization", format!("Bearer {}", api_key))
        .header("Content-Type", "application/json")
        .header("HTTP-Referer", "https://github.com/finnmagnuskverndalen/airpulse")
        .json(&json!({
            "model": model,
            "max_tokens": 200,
            "messages": [{"role": "user", "content": prompt}]
        }))
        .send()
        .await;

    match res {
        Ok(r) => {
            match r.json::<serde_json::Value>().await {
                Ok(j) => j["choices"][0]["message"]["content"]
                    .as_str()
                    .unwrap_or("")
                    .trim()
                    .to_string(),
                Err(e) => { tracing::warn!("LLM parse error: {}", e); String::new() }
            }
        }
        Err(e) => { tracing::warn!("LLM request error: {}", e); String::new() }
    }
}
