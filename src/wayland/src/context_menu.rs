use jfn_platform_abi::{
    ContextMenuBackend, ContextMenuStyle, DisplayBackend, JfnContextMenuRequest, JsMenuContextMenu,
    context_menu_style,
};

pub(crate) fn backend() -> &'static dyn ContextMenuBackend {
    match context_menu_style(DisplayBackend::Wayland) {
        ContextMenuStyle::PlatformMenu => &XdgPopupContextMenu,
        ContextMenuStyle::JsMenu => &JsMenuContextMenu,
    }
}

struct XdgPopupContextMenu;

impl ContextMenuBackend for XdgPopupContextMenu {
    fn show(&self, req: JfnContextMenuRequest) {
        let items = req
            .items
            .into_iter()
            .map(|i| jfn_menu::MenuItem {
                id: i.id,
                label: i.label,
                enabled: i.enabled,
                separator: i.separator,
            })
            .collect();
        let cb = req.on_selected.unwrap_or_else(|| Box::new(|_| {}));
        crate::popup::show(items, req.x, req.y, cb);
    }
}
