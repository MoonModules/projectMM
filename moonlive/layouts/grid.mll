// A grid, the layout almost every panel is.

class GridLayout {
  byte cols = 16;
  byte rows = 16;

  int dimensions() { return 2; }

  string tags() { return "💫"; }

  void defineControls() {
    addControl("cols", cols, 1, 128); // lights across
    addControl("rows", rows, 1, 128); // lights down
  }

  void placeLights() {
    for (int y = 0; y < rows; y = y + 1) {
      for (int x = 0; x < cols; x = x + 1) {
        addLight(x, y, 0);
      }
    }
  }
}
