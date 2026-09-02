// Rose: the strand traces a rhodonea curve, a circle whose radius swells and collapses

class RoseLayout {
  byte petals = 2;
  byte radius = 15;

  int dimensions() { return 2; }

  string tags() { return "💫"; }

  void defineControls() {
    addControl("petals", petals, 1, 8); // how many petals the curve draws
    addControl("radius", radius, 4, 30); // how far the petals reach
  }

  void placeLights() {
    for (int i = 0; i < 256; i = i + 1) {
      addLight(radius - scale(sin(i * turn(256) * petals), radius + 1)
                 + scale(cos(i * turn(256)),
                         2 * scale(sin(i * turn(256) * petals), radius + 1) + 1),
               radius - scale(sin(i * turn(256) * petals), radius + 1)
                 + scale(sin(i * turn(256)),
                         2 * scale(sin(i * turn(256) * petals), radius + 1) + 1),
               0);
    }
  }
}
