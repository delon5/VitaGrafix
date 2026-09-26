# VitaGrafix
VitaGrafix is a taiHEN plugin that allows you to change resolution and FPS cap of PS Vita games (to get better visuals, higher FPS or longer battery life).

**Official patchlist:** [HERE](https://github.com/Electry/VitaGrafixPatchlist)

**For more information, visit the VitaGrafix wiki:** [HERE](https://github.com/Electry/VitaGrafix/wiki)

## In-game menu
When a game with a matching patch is running (and `OSD` is on, the default), hold **SELECT** and press **R** to open the menu. It shows only the options the game's patch supports.

- **UP / DOWN** select an option, **LEFT / RIGHT** change it, **CROSS** turns it on or off
- **IB** switches between the framebuffer sizes (960x544, 720x408, 640x368, 480x272), one resolution for every internal resolution the game uses; a different value from the config stays selectable too
- **START** saves and closes, **SELECT + R** closes without saving

While the menu is open the game does not receive any buttons, sticks or touch. Saved settings apply the next time the game starts.

## Config files
Each game's settings live in its own file, `ux0:data/VitaGrafix/config/<TITLEID>.txt`. The in-game menu and VitaGrafixConfigurator save there (the folder is created on the first save); the options can be listed without a section header, since the file belongs to that game.

When a game has its own file, VitaGrafix uses it instead of `config.txt`, the way the `patch` folder is preferred over `patchlist.txt`. `config.txt` is only read for games that have no file of their own, and nothing writes to it any more. A game's first save copies what it used from `config.txt` (including `LOG`) into its new file. Delete the file to go back to `config.txt` for that game.

If a config file cannot be read, the error on screen names the file and line.

<br>
<br>

_____

Licensed under [GNU GPL v3](https://github.com/Electry/VitaGrafix/blob/master/LICENSE.md)
