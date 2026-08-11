This configuration creates a new workspace when you reach the last populated workspace on a monitor and scroll down again. The custom `mouse_down` bind is enabled only in that situation; at other times it remains disabled, allowing ScrollOverview's built-in scrolling action to run normally.

The example also ignores special and empty workspaces. Moving to `emptynm` lets Hyprland select the next empty workspace on the monitor.

Add the state and helper functions to your Lua configuration:

```lua
local lastWorkspaceScrollBind

local function table_value(value, ...)
    if value == nil then
        return nil
    end

    for _, key in ipairs({ ... }) do
        local ok, item = pcall(function()
            return value[key]
        end)

        if ok and item ~= nil then
            return item
        end
    end

    return nil
end

local function last_workspace_state()
    local monitor = hl.get_monitor_at_cursor() or hl.get_active_monitor()
    if not monitor then
        return false, nil
    end

    local activeWorkspace = table_value(monitor, "active_workspace")
    local activeWorkspaceId = table_value(activeWorkspace, "id")
    if type(activeWorkspaceId) ~= "number" or activeWorkspaceId <= 0 then
        return false, nil
    end

    local activeWorkspaceWindows = table_value(activeWorkspace, "windows")
    if type(activeWorkspaceWindows) ~= "number" or activeWorkspaceWindows == 0 then
        return false, nil
    end

    local lastWorkspaceId
    for _, workspace in ipairs(hl.get_workspaces()) do
        local id = table_value(workspace, "id")

        if type(id) == "number"
            and id > 0
            and table_value(workspace, "special") ~= true
            and table_value(workspace, "monitor") == monitor then
            lastWorkspaceId = math.max(lastWorkspaceId or id, id)
        end
    end

    return activeWorkspaceId == lastWorkspaceId, monitor
end

local function create_workspace_at_end()
    local isLastWorkspace, monitor = last_workspace_state()
    if not isLastWorkspace or not monitor then
        return
    end

    hl.dispatch(hl.dsp.focus({ monitor = table_value(monitor, "name") }))
    hl.dispatch(hl.dsp.focus({ workspace = "emptynm" }))
end

local function update_last_workspace_scroll_bind()
    if not lastWorkspaceScrollBind then
        return
    end

    local enabled = false
    if hl.get_current_submap() == "scrolloverview" then
        enabled = last_workspace_state()
    end

    lastWorkspaceScrollBind:set_enabled(enabled)
end
```

Add the following bind inside your existing `scrolloverview` submap. Do not define the same submap a second time; merge this with the configuration from [Keybind submap](Keybind-submap.md).

```lua
lastWorkspaceScrollBind = hl.bind("mouse_down", create_workspace_at_end)
lastWorkspaceScrollBind:set_enabled(false)
```

Finally, update the bind whenever the workspace, window, monitor, or active submap changes. The timer also keeps the state current when the pointer moves between monitors without triggering one of those events.

```lua
hl.on("workspace.active", update_last_workspace_scroll_bind)
hl.on("workspace.created", update_last_workspace_scroll_bind)
hl.on("workspace.removed", update_last_workspace_scroll_bind)
hl.on("workspace.move_to_monitor", update_last_workspace_scroll_bind)
hl.on("window.open", update_last_workspace_scroll_bind)
hl.on("window.close", update_last_workspace_scroll_bind)
hl.on("window.move_to_workspace", update_last_workspace_scroll_bind)
hl.on("monitor.focused", update_last_workspace_scroll_bind)
hl.on("keybinds.submap", update_last_workspace_scroll_bind)

local lastWorkspaceScrollTimer = hl.timer(update_last_workspace_scroll_bind, {
    timeout = 50,
    type = "repeat",
})

update_last_workspace_scroll_bind()
```

[Back to Advanced configuration](Advanced-configuration.md)
