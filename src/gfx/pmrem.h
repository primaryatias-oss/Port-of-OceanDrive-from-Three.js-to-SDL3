// PMREMGenerator.fromScene: prefiltered environment as a cube-UV atlas texture.
#pragma once

#include "gfx/scene.h"

Texture *pmrem_from_scene(Node *scene, double near, double far, int size);
