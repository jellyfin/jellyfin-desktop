import QtQuick
import QtWebEngine
import QtTest

TestCase {
    id: testCase
    name: "DesktopNavigation"
    when: windowShown
    width: 640
    height: 480
    property bool webReady: false

    WebEngineView {
        id: web
        anchors.fill: parent
        url: Qt.resolvedUrl("keyboard-fixture.html")
        onLoadingChanged: function(info) {
            if (info.status === WebEngineView.LoadSucceededStatus)
                testCase.webReady = true
        }
    }

    function evaluate(script) {
        var completed = false
        var result
        web.runJavaScript(script, function(value) {
            result = value
            completed = true
        })
        tryVerify(function() { return completed }, 2000)
        return result
    }

    function initTestCase() {
        tryCompare(testCase, "webReady", true, 10000)
        compare(evaluate("typeof window._inputPlugin"), "function")
    }

    function init() {
        evaluate("resetFixture()")
        web.forceActiveFocus()
        tryCompare(web, "activeFocus", true)
        wait(50)
    }

    function test_page_keys_navigate_once_data() {
        return [{tag: "escape", key: Qt.Key_Escape},
                {tag: "backspace", key: Qt.Key_Backspace}]
    }

    function test_page_keys_navigate_once(data) {
        keyClick(data.key)
        wait(100)
        compare(evaluate("commands.length"), 1)
        compare(evaluate("commands[0]"), "back")
    }

    function test_editors_keep_backspace_data() {
        return [{tag: "input", id: "editor", value: "value"},
                {tag: "textarea", id: "textarea", value: "value"},
                {tag: "contenteditable", id: "contenteditable", value: "textContent"}]
    }

    function test_editors_keep_backspace(data) {
        compare(evaluate("focusEditor('" + data.id + "')"), data.id)
        keyClick(Qt.Key_Backspace)
        wait(100)
        compare(evaluate("document.getElementById('" + data.id + "')." + data.value), "ab")
        compare(evaluate("commands.length"), 0)
    }

    function test_modal_escape_dismisses_without_navigation() {
        compare(evaluate("document.getElementById('modal').showModal(); document.getElementById('modal').open"), true)
        keyClick(Qt.Key_Escape)
        wait(100)
        compare(evaluate("document.getElementById('modal').open"), false)
        compare(evaluate("commands.length"), 0)
    }

    function test_consumed_key_is_not_navigation() {
        evaluate("cancelKey = true")
        keyClick(Qt.Key_Escape)
        compare(evaluate("commands.length"), 0)
    }

    function test_client_settings_dialog_is_not_navigation() {
        evaluate("document.body.insertAdjacentHTML('beforeend', '<div class=dialogContainer><div class=\"dialog opened\"><button id=dialogButton>Close</button></div></div>'); document.getElementById('dialogButton').focus()")
        keyClick(Qt.Key_Escape)
        keyClick(Qt.Key_Backspace)
        compare(evaluate("commands.length"), 0)
    }

    function test_other_modes_keep_existing_handling() {
        evaluate("jmpInfo.settings.main.webMode = 'tv'")
        keyClick(Qt.Key_Escape)
        keyClick(Qt.Key_Backspace)
        compare(evaluate("commands.length"), 0)
    }

    function test_fullscreen_escape_is_not_navigation() {
        evaluate("jmpInfo.settings.main.fullscreen = true")
        keyClick(Qt.Key_Escape)
        compare(evaluate("commands.length"), 0)
    }

    function test_modifiers_repeat_and_composition_are_not_navigation() {
        evaluate("['ctrlKey','metaKey','altKey','shiftKey','repeat','isComposing'].forEach(flag => document.body.dispatchEvent(new KeyboardEvent('keydown', {key:'Backspace', bubbles:true, cancelable:true, [flag]:true})))")
        compare(evaluate("commands.length"), 0)
    }

    function test_destroy_removes_listener() {
        evaluate("plugin.destroy()")
        keyClick(Qt.Key_Backspace)
        compare(evaluate("commands.length"), 0)
    }
}
