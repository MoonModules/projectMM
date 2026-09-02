// A 3D lattice: stacked layers of a grid, the primitive 3D space of LED strips.

class LatticeLayout {
  byte cols = 4;
  byte rows = 3;
  byte layers = 5;

  int dimensions() { return 3; }

  string tags() { return "💫"; }

  void defineControls() {
    addControl("cols", cols, 1, 32); // lights across
    addControl("rows", rows, 1, 32); // lights down
    addControl("layers", layers, 1, 32); // grids stacked in depth
  }

  void placeLights() {
    for (int z = 0; z < layers; z = z + 1) {
      for (int y = 0; y < rows; y = y + 1) {
        for (int x = 0; x < cols; x = x + 1) {
          addLight(x, y, z);
        }
      }
    }
  }
}
