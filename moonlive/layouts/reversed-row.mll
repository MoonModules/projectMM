// A strand wired right to left: light 0 sits at the far end.

class ReversedRowLayout {
  byte cols = 16;

  int dimensions() { return 1; }

  string tags() { return "💫"; }

  void defineControls() {
    addControl("cols", cols, 1, 64); // how many lights the strand has
  }

  void placeLights() {
    for (int i = 0; i < cols; i = i + 1) {
      addLight(cols - 1 - i, 0, 0);
    }
  }
}
