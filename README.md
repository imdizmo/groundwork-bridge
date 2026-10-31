# Groundwork Bridge

An Unreal Engine editor plugin that links the open level to [Groundwork](https://github.com/imdizmo/groundwork), a
map editor for Unreal: what you change in Groundwork (ground, painted layers, objects, rivers and lakes) lands in the
level as you make it, and what you change in the level comes back.

It does nothing on its own: it waits for Groundwork's **Live** switch.

## Install

Built and tested with Unreal Engine 5.8. The plugin needs the engine's **Water** plugin (it makes the rivers and
lakes).

Build it for your engine, then copy it into your project's `Plugins` folder:

```sh
/path/to/UnrealEngine/Engine/Build/BatchFiles/RunUAT.sh BuildPlugin \
  -Plugin=/path/to/groundwork-bridge/GroundworkBridge.uplugin \
  -Package=/tmp/GroundworkBridge -TargetPlatforms=Linux     # Win64 or Mac on those (RunUAT.bat on Windows)
cp -r /tmp/GroundworkBridge /path/to/YourProject/Plugins/GroundworkBridge
```

In a C++ project you can instead clone this repository into `Plugins/GroundworkBridge` and let the editor build it.

Then name the project in your map's `map.json` (`"unreal": { "project": "/path/to/YourProject" }`), open the level in
Unreal, and turn on **Live** in Groundwork. See Groundwork's [Unreal page](https://github.com/imdizmo/groundwork/blob/main/docs/unreal.md)
for the map side: the layers' texture, the water bodies' prefix, the models' Unreal meshes.

## What it does

When the Unreal editor starts, the plugin listens on `127.0.0.1:5772`, this computer only, and writes a fresh token to
`Saved/GroundworkBridge.json` (removed when Unreal closes). Only callers on this computer with that token are
answered. Nothing is saved: the level is marked changed, as your own edits would mark it, and you save it in Unreal.

The protocol (version 1; Groundwork refuses a bridge of another version):

| Route | What it does |
|---|---|
| `GET /groundwork/ping` | `{"bridge": "groundwork", "version": 1}` |
| `POST /groundwork/heights` | A rectangle of map pixels' heights (metres) into the landscape's first edit layer. |
| `POST /groundwork/layers` | A rectangle of each layer's weights: into a channel of the texture the landscape material reads (named by prefix; exactly one must match), or into the landscape's paint layer of the same name. |
| `POST /groundwork/objects` | Static meshes added and removed (as instances of the level's instanced components where there are some, else as actors), and marker actors of a given class. |
| `POST /groundwork/water` | The map's rivers and lakes: water bodies labelled prefix + name rewritten in place, new ones made like the first of their kind, gone ones deleted. Only river and lake bodies with the prefix are touched. |
| `GET /groundwork/version` | Counters that go up when the level is changed by hand (ground, objects, water). |
| `GET /groundwork/heights_all`, `objects_all`, `water_all` | The landscape's heights, the level's meshes and markers, its rivers and lakes, for Groundwork to follow. |

Positions are Unreal's centimetres in the routes that read the level, and the map's metres in those that write it.

It also gives scripts `UGroundworkLandscapeLibrary` (Blueprint and Python): `CreateLandscapeFromRaw` and
`ReplaceLandscapeFromRaw`, a landscape of any size from a raw heightmap, as the editor's own New Landscape and resize
make one.

## License

MIT (see [LICENSE](LICENSE)).
