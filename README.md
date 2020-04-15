# RayGen Renderer

A simple cross-platform ray tracing engine for 3D graphics rendering.

# Snapshots

![cube room](resources/scenes/cubeRoom/sample.jpg)<br />
the cube room

![cube room](resources/scenes/cubeRoom/cubeRoom_reflection.jpg)<br />
the cube room (reflection version)

![sphere array](resources/scenes/sphereArray/sample.jpg)<br />
spheres & materials

![suzanne](resources/scenes/suzanne/Suzanne%20Texture.jpg)<br />
suzanne glass

![suzanne](resources/scenes/suzanne/Suzanne%20Jade.jpg)<br />
suzanne jade

# Build 

The following build script or projects are available from this respositry.


| Platform | Folder | Build tool |
|---|---|---|
| macOS | build/mac | GNU make |
| macOS | projects/raygen.xcodeproj | Xcode |
| linux | build/linux | GNU make |
| Windows | projects/raygen-win32/raygen.sln | Visual Studio |

Example to use GNU make on linux and macOS platform:

```shell
cd build/<platform>
make
```

# Usage

```shell
$ ./raygen <command> <sceneFile.json> [output_image] [-options]
```
e.g.:
```shell
$ ./raygen render ../../resources/scenes/cubeRoom/cubeRoom.json -s 100
```

## Options

| option | description |
| --- | --- |
| -r \| --resolution | resolution of result image |
| -s \| --samples | number of samples (rays from camera) |
| -c \| --cores | --threads | number of threads/cores to render parallelly |
| -ds \| --dofs | --dof-samples | number of samples on depth of field calculation |
| -enaa \| --enable-antialias | enable antialias (default: on) |
| -encs \| --enable-color-sampling | enable sample colors from texture (default: on) |
| -enpp \| --enable-postprocess | eanble post-processes (grow and blur) |
| -d \| --shader | shader system (see below) |
| --focus-obj | automatically make camera look at a specified object |
| --dump | dump scene define |

## Scene

Raygen scene is described by a JSON file using the following structure.

scene.json:
```js
{
  obj1: {
    location: [x, y, z],
    angle: [x, y, z],
    scale: [x, y, z],
    mesh: "path/to/mesh.mesh",
    mat: {
      color: [r, g, b] or "#d0d0d0",
      tex: "path/to/texture.png",
      glossy: 0 ~ 1,
      roughness: 0 ~ 1,
      transparency: 0 ~ 1,
      refraction: 0 ~ 1,
      ...,
    }
  },
  obj2: {
    ...
  },
  ...
}
```

The several sample scenes can be found inside `resources/scenes` folder.

- **Cube Room** - A well-known two cubes scene that is usually used to benchmark a ray-tracing engine
- **Sphere Array** - A scene which has five sphere to test different materials
- **suzanne** - A model suzanne from blender used to test transparency and refraction material

# Dependency modules

The following modules are required to build this software. They can be found inside `/inc` folder as git submodules.

- C++ Common Module (https://github.com/jingwood/cpp-common-class)
- C++ Graphics Module (https://github.com/jingwood/cpp-graphics-module)

# License

Released under MIT License.

Copyright © Jingwood, unvell.com, all Rights Reserved.
