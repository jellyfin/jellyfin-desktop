use jfn_platform_abi::{
    ContextMenuBackend, ContextMenuStyle, DisplayBackend, JfnContextMenuRequest, JsMenuContextMenu,
    context_menu_style,
};

use crate::ns_menu::{MenuEntry, MenuSpec, present_on_main};

pub(crate) fn backend() -> &'static dyn ContextMenuBackend {
    match context_menu_style(DisplayBackend::MacOS) {
        ContextMenuStyle::PlatformMenu => &NsMenuContextMenu,
        ContextMenuStyle::JsMenu => &JsMenuContextMenu,
    }
}

struct NsMenuContextMenu;

impl ContextMenuBackend for NsMenuContextMenu {
    fn show(&self, req: JfnContextMenuRequest) {
        if req.items.is_empty() {
            return;
        }
        let entries = req
            .items
            .into_iter()
            .map(|it| MenuEntry {
                title: it.label,
                tag: it.id,
                enabled: it.enabled,
                separator: it.separator,
                checked: false,
            })
            .collect();
        let spec = MenuSpec {
            entries,
            x: req.x,
            y: req.y,
            positioning_tag: None,
            min_width: None,
        };
        present_on_main(spec, req.on_selected);
    }
}
