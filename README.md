<div align="center">
  <h2>Iris Theme</h2>
  <h3>A Material You palette generator for Iris Environment.</h3>
  <h5>This page will help you set up Iris Theme for your Iris Environment.</h5>
</div>

## About
**Iris Theme** is a customizable Material You palette generator made for Iris, inspired by [Matugen](https://github.com/InioX/matugen) made with
around the same template files and keys, so you won't be having difficulties moving from Matugen to Iris Theme.

## Dependencies
```bash
cmake
ninja
git
```

## Installing
> [!NOTE]
> Only manual installation works for now, Iris Theme isn't available at any Linux repositories for now.

### Manual Installation
```bash
mkdir -p /tmp/Iris-Theme
cd /tmp/Iris-Theme
git clone https://github.com/sol-less/Iris-Theme
cd Iris-Theme/
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/iris-theme image <PATH/TO/IMAGE> # for testing purposes only
```

### System-wide Installation
(after building)
```bash
sudo cmake --install build
```

## Running/usage
```bash
iris-theme image <PATH/TO/IMAGE>
```

## Customizing
Configuration file could be located at `~/.config/iris-theme/config.toml`, this config shares almost the same foundation for Matugen config file.
With an addition of `json_out`, it is by default activated and can be set its output. Can disable by setting `json_out = ""` in `config.toml`

### Example config
```toml
[config]
default_scheme = "scheme-expressive"
default_mode = "dark"
json_out = "~/.local/state/quickshell/generated/colors.json"   # your shell's Theme; "" to disable

[config.wallpaper]
set = true
command = "awww"
arguments = ["img", "--transition-type", "grow"]      # the image path is appended last

[templates.gtk4]
input_path = "~/.config/iris-theme/templates/gtk.css"
output_path = "~/.config/gtk-4.0/gtk.css"

[templates.gtk3]
input_path = "~/.config/iris-theme/templates/gtk.css"
output_path = "~/.config/gtk-3.0/gtk.css"
post_hook = 'gsettings set org.gnome.desktop.interface color-scheme "prefer-$IRIS_THEME_MODE"'
```
