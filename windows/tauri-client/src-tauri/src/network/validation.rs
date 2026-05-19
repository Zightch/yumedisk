use crate::api_error::ApiError;

pub fn validate_server_addr(server_addr: &str) -> Result<&str, ApiError> {
    let server_addr = server_addr.trim();
    if server_addr.is_empty() {
        return Err(ApiError::new(
            "network-server-addr-empty",
            "服务器地址不能为空",
            None,
        ));
    }

    Ok(server_addr)
}

pub fn validate_disk_name(disk_name: &str) -> Result<&str, ApiError> {
    let disk_name = disk_name.trim();
    if disk_name.is_empty() {
        return Err(ApiError::new("invalid-disk-name", "磁盘名称不能为空", None));
    }

    Ok(disk_name)
}

pub fn draft_not_found_error(draft_id: &str) -> ApiError {
    ApiError::new(
        "network-draft-not-found",
        "网络盘草稿不存在",
        Some(draft_id.to_string()),
    )
}

pub fn draft_item_not_found_error(remote_disk_id: &str) -> ApiError {
    ApiError::new(
        "network-draft-item-not-found",
        "网络盘草稿项不存在",
        Some(remote_disk_id.to_string()),
    )
}

pub fn disk_not_found_error(local_disk_id: &str) -> ApiError {
    ApiError::new(
        "disk-not-found",
        "磁盘不存在",
        Some(local_disk_id.to_string()),
    )
}
