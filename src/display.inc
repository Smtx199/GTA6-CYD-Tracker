static uint8_t drawBuf[W * 8 * 2] __attribute__((aligned(4)));   // 5 Ko : la RAM est précieuse

static void flush_cb(lv_display_t* disp, const lv_area_t* a, uint8_t* px) {
  tft.pushImage(a->x1, a->y1, a->x2 - a->x1 + 1, a->y2 - a->y1 + 1, (lgfx::rgb565_t*)px);
  lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t*, lv_indev_data_t* d) {
  static int32_t lx = 0, ly = 0;
  static uint32_t lastDown = 0;
  int32_t x, y;
  uint32_t now = millis();
  if (tft.getTouch(&x, &y)) {
    lx = x; ly = y; lastDown = now;
    d->state = LV_INDEV_STATE_PRESSED;
    d->point.x = x;
    d->point.y = y;
  } else if (lastDown && now - lastDown < 70) {
    // Anti-rebond : on garde l'appui 70 ms après une perte de contact,
    // pour éviter qu'un glissement soit pris pour un clic
    d->state = LV_INDEV_STATE_PRESSED;
    d->point.x = lx;
    d->point.y = ly;
  } else {
    d->state = LV_INDEV_STATE_RELEASED;
  }
}

static uint32_t tick_cb() { return millis(); }
