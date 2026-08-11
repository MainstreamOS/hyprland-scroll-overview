## Using Hyprpm (recommended)

1. For stable releases of Hyprland, add the plugin repository:

   ```bash
   hyprpm add https://github.com/yayuuu/hyprland-scroll-overview.git
   ```

   If you use a Git build of Hyprland, add the plugin from the `new-release` branch instead:

   ```bash
   hyprpm add https://github.com/yayuuu/hyprland-scroll-overview origin/new-release
   ```

2. Build and fetch dependencies:

   ```bash
   hyprpm update
   ```

3. Enable the plugin:

   ```bash
   hyprpm enable scrolloverview
   ```

4. To load enabled plugins automatically when Hyprland starts, run `hyprpm reload` from the `hyprland.start` event handler in your Lua configuration:

   ```lua
   hl.on("hyprland.start", function()
       hl.exec_cmd("hyprpm reload")
   end)
   ```

5. Configure and enjoy.

## Building from source

1. Clone the repository and enter its directory:

   ```bash
   git clone https://github.com/yayuuu/hyprland-scroll-overview.git
   cd hyprland-scroll-overview
   ```

2. Build the plugin:

   ```bash
   make
   ```

3. Load the plugin into the running Hyprland instance:

   ```bash
   hyprctl plugin load "$(pwd)/scrolloverview.so"
   ```

4. To load the plugin automatically when Hyprland starts, add it to your Lua configuration using an absolute path:

   ```lua
   local pluginPath = "/absolute/path/to/hyprland-scroll-overview/scrolloverview.so"
   hl.plugin.load(pluginPath)
   ```
