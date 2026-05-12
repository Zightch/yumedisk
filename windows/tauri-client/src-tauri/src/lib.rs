mod api_error;
mod backend;
mod commands;
mod state;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .manage(state::client_state::ClientState::default())
        .plugin(tauri_plugin_opener::init())
        .invoke_handler(tauri::generate_handler![commands::session::initialize_client])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
