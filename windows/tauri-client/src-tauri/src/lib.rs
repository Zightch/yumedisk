mod api_error;
mod backend;
mod commands;
mod single_instance;
mod state;

use tauri::image::Image;
use tauri::menu::{MenuBuilder, MenuEvent, MenuItem};
use tauri::tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent};
use tauri::{AppHandle, Manager, WindowEvent};

const MAIN_WINDOW_LABEL: &str = "main";
const TRAY_OPEN_ID: &str = "tray-open-main-window";
const TRAY_EXIT_ID: &str = "tray-exit-app";

fn show_main_window(app: &AppHandle) {
    if let Some(window) = app.get_webview_window(MAIN_WINDOW_LABEL) {
        let _ = window.show();
        let _ = window.unminimize();
        let _ = window.set_focus();
    }
}

fn build_tray(app: &AppHandle) -> tauri::Result<()> {
    let open_item = MenuItem::with_id(app, TRAY_OPEN_ID, "打开主窗口", true, None::<&str>)?;
    let exit_item = MenuItem::with_id(app, TRAY_EXIT_ID, "退出", true, None::<&str>)?;
    let menu = MenuBuilder::new(app)
        .item(&open_item)
        .item(&exit_item)
        .build()?;

    let tray_icon = app
        .default_window_icon()
        .cloned()
        .or_else(|| Image::from_path("icons/32x32.png").ok());

    let mut tray = TrayIconBuilder::with_id("main-tray")
        .menu(&menu)
        .show_menu_on_left_click(false)
        .tooltip("YumeDisk");

    if let Some(icon) = tray_icon {
        tray = tray.icon(icon);
    }

    tray.on_menu_event(|app, event: MenuEvent| match event.id().as_ref() {
        TRAY_OPEN_ID => show_main_window(app),
        TRAY_EXIT_ID => {
            if let Some(state) = app.try_state::<state::client_state::ClientState>() {
                state.mark_exiting();
            }
            app.exit(0);
        }
        _ => {}
    })
    .on_tray_icon_event(|tray, event| {
        if let TrayIconEvent::DoubleClick {
            button: MouseButton::Left,
            ..
        }
        | TrayIconEvent::Click {
            button: MouseButton::Left,
            button_state: MouseButtonState::Up,
            ..
        } = event
        {
            show_main_window(&tray.app_handle());
        }
    })
    .build(app)?;

    Ok(())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let single_instance_guard = match single_instance::initialize() {
        single_instance::StartupAction::Continue(guard) => guard,
        single_instance::StartupAction::Exit => return,
    };

    tauri::Builder::default()
        .manage(single_instance_guard)
        .manage(state::client_state::ClientState::default())
        .plugin(tauri_plugin_opener::init())
        .setup(|app| {
            build_tray(app.handle())?;
            let single_instance_guard = app.state::<single_instance::SingleInstanceGuard>();
            single_instance::spawn_wake_listener(
                app.handle(),
                &single_instance_guard,
                show_main_window,
            );
            Ok(())
        })
        .on_window_event(|window, event| {
            if window.label() != MAIN_WINDOW_LABEL {
                return;
            }

            if let WindowEvent::CloseRequested { api, .. } = event {
                let Some(state) = window
                    .app_handle()
                    .try_state::<state::client_state::ClientState>()
                else {
                    return;
                };

                if state.is_exiting() {
                    return;
                }

                api.prevent_close();
                let _ = window.hide();
            }
        })
        .invoke_handler(tauri::generate_handler![
            commands::app_info::query_component_versions,
            commands::app_session::restore_client_state,
            commands::app_session::open_app_session,
            commands::config::query_backend_defaults,
            commands::config::query_app_session_config,
            commands::disk::query_managed_disks,
            commands::disk::query_home_disk_list,
            commands::disk::rescan_runtime_disks,
            commands::disk::create_memory_disk,
            commands::disk::pick_raw_file_path,
            commands::disk::pick_new_raw_file_path,
            commands::disk::create_file_disk,
            commands::disk::create_new_file_disk,
            commands::disk::mount_disk,
            commands::disk::eject_disk,
            commands::disk::delete_disk,
            commands::disk::undo_delete_disk,
            commands::disk::commit_deleted_disk,
            commands::disk::update_disk
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
