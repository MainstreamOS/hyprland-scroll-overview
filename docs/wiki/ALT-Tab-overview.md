ScrollOverview can act as a visual `ALT + Tab` switcher. Pressing `ALT + Tab` opens a compact horizontal overview and uses the plugin's `navigate("right")` dispatcher to advance to the next column. Navigation stays on the monitor where the overview was opened and skips windows that are both pinned and floating.

The dispatcher does not wrap at the end. The module therefore compares the active workspace and window before and after moving right. If neither changes, the selection is already at the last column on that monitor, so it navigates left until it reaches the first workspace and its first column.

Create `~/.config/hypr/scripts/alttab.lua` with the following module:

```lua
local M = {}

local openedByAltTab = false

local function object_value(value, key)
    if value == nil then
        return nil
    end

    local ok, result = pcall(function()
        return value[key]
    end)

    return ok and result or nil
end

local function selection_state()
    local workspaceId = object_value(hl.get_active_workspace(), "id")
    local windowAddress = object_value(hl.get_active_window(), "address")
    return tostring(workspaceId) .. ":" .. tostring(windowAddress)
end

local function navigate_to_first_column()
    local previousState
    local maxSteps = #(hl.get_windows() or {}) + #(hl.get_workspaces() or {}) + 1

    for _ = 1, maxSteps do
        local currentState = selection_state()
        if currentState == previousState then
            return
        end

        previousState = currentState
        hl.plugin.scrolloverview.navigate("left")
    end
end

local function navigate_next_column()
    local previousState = selection_state()
    hl.plugin.scrolloverview.navigate("right")

    if selection_state() == previousState then
        navigate_to_first_column()
    end
end

function M.next()
    hl.config({
        plugin = {
            scrolloverview = {
                layout = "horizontal",
                scale = 0.3,
            },
        },
    })
    hl.plugin.scrolloverview.overview("on")
    openedByAltTab = true
    navigate_next_column()
end

function M.close()
    if openedByAltTab then
        hl.plugin.scrolloverview.overview("off")
        openedByAltTab = false
    end
end

return M
```

Load the module and add the keybinds to `~/.config/hypr/hyprland.lua`:

```lua
local altTab = require("scripts.alttab")

hl.bind("ALT + Tab", altTab.next, { submap_universal = true })
hl.bind("ALT + Alt_L", altTab.close, { release = true, transparent = true })
hl.bind("ALT + Alt_R", altTab.close, { release = true, transparent = true })
```

[Back to Advanced configuration](Advanced-configuration.md)
