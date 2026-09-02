// Two rows from one strand: out along y=0, back along y=1.

class TwoRowsLayout {
  byte cols = 16;

  int dimensions() { return 2; }

  string tags() { return "💫"; }

  void defineControls() {
    addControl("cols", cols, 1, 64); // lights in each row
  }

  void placeLights() {
    for (int i = 0; i < cols; i = i + 1) {
      addLight(i, 0, 0);
    }
    for (int i = 0; i < cols; i = i + 1) {
      addLight(cols - 1 - i, 1, 0);
    }
  }
}
