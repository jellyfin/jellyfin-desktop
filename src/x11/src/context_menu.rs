use jfn_platform_abi::{
    ContextMenuBackend, ContextMenuStyle, Delivery, DisplayBackend, JfnContextMenuRequest,
    JsMenuContextMenu, context_menu_style,
};

pub(crate) fn backend() -> &'static dyn ContextMenuBackend {
    match context_menu_style(DisplayBackend::X11) {
        ContextMenuStyle::PlatformMenu => &MenuWindowContextMenu,
        ContextMenuStyle::JsMenu => &JsMenuContextMenu,
    }
}

struct MenuWindowContextMenu;

impl ContextMenuBackend for MenuWindowContextMenu {
    fn show(&self, req: JfnContextMenuRequest) {
        let Delivery::Native(on_selected) = req.delivery else {
            debug_assert!(false, "MenuWindowContextMenu requires Delivery::Native");
            return;
        };
        let items = req
            .items
            .into_iter()
            .map(|i| crate::menu::MenuItem {
                id: i.id,
                label: i.label,
                enabled: i.enabled,
                separator: i.separator,
            })
            .collect();
        crate::menu::show(crate::menu::MenuRequest {
            x: req.x,
            y: req.y,
            items,
            on_selected: Some(on_selected),
        });
    }
}
