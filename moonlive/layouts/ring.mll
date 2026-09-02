// A circle: `count` lights evenly around a center, spanning 2*radius+1 cells.

class RingLayout {
  int count = 24;
  byte radius = 5;

  int dimensions() { return 2; }

  string tags() { return "💫"; }

  void defineControls() {
    addControl("count", count, 3, 1000); // lights evenly around the circle
    addControl("radius", radius, 1, 127); // how wide the circle is
  }

  void placeLights() {
    for (int i = 0; i < count; i = i + 1) {
      addLight(scale(cos(i * turn(count)), radius * 2 + 1),
               scale(sin(i * turn(count)), radius * 2 + 1), 0);
    }
  }
}
