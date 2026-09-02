// A diagonal run: light i at (i, i). The kind of fixture that otherwise needs its own class.

class DiagonalLayout {
  byte count = 16;

  int dimensions() { return 2; }

  string tags() { return "💫"; }

  void defineControls() {
    addControl("count", count, 1, 64); // how many lights the run has
  }

  void placeLights() {
    for (int i = 0; i < count; i = i + 1) {
      addLight(i, i, 0);
    }
  }
}
