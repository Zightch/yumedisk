use serde::Serialize;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ApiError {
    pub code: String,
    pub message: String,
    pub detail: Option<String>,
}

impl ApiError {
    pub fn new(code: impl Into<String>, message: impl Into<String>, detail: Option<String>) -> Self {
        Self {
            code: code.into(),
            message: message.into(),
            detail,
        }
    }
}
