The `scrolling_mode` option can be changed at runtime. This example uses the normal, layout-aware mode by default (for example scrolling changes workspace) and switches to the inverted mode while either Shift key is held (to scrolling through the columns within a workspace). Releasing Shift restores the original scrolling behavior.

Add the following state and helper functions to your Lua configuration:

```lua
local shiftPressed = {
    left = false,
    right = false,
}

local function set_scrolling_mode(mode)
    hl.config({
        plugin = {
            scrolloverview = {
                input = {
                    scrolling_mode = mode,
                },
            },
        },
    })
end

local function reset_scrolling_mode()
    shiftPressed.left = false
    shiftPressed.right = false
    set_scrolling_mode(0)
end

local function set_shift_pressed(side, pressed)
    shiftPressed[side] = pressed
    set_scrolling_mode((shiftPressed.left or shiftPressed.right) and 1 or 0)
end

reset_scrolling_mode()
```

Add these bindings inside your existing `scrolloverview` submap. Both left and right Shift are tracked so the mode is restored only after every held Shift key has been released. The release binds are universal, allowing them to run even if the overview closes before Shift is released.

```lua
hl.bind("Shift_L", function()
    set_shift_pressed("left", true)
end, { transparent = true, non_consuming = true })

hl.bind("Shift_R", function()
    set_shift_pressed("right", true)
end, { transparent = true, non_consuming = true })

hl.bind("SHIFT + Shift_L", function()
    set_shift_pressed("left", false)
end, {
    release = true,
    transparent = true,
    non_consuming = true,
    submap_universal = true,
})

hl.bind("SHIFT + Shift_R", function()
    set_shift_pressed("right", false)
end, {
    release = true,
    transparent = true,
    non_consuming = true,
    submap_universal = true,
})
```

Do not define the `scrolloverview` submap again. Merge these bindings into the callback shown on [Keybind submap](Keybind-submap.md).

[Back to Advanced configuration](Advanced-configuration.md)
