/*
  QuinLED Dig-Quad — 8 световых программ + веб-управление
  =========================================================
  Программы (выбираются с телефона через браузер):
   1. Бегущая линия (4 фазы: одна/три точки, туда/обратно)
   2. Движение парами рёбер снизу вверх
   3. Случайные вспышки квадратов/прямоугольников (все размеры)
   4. Случайные вспышки ломаных фигур
   5. Марш прямоугольников и квадратов вверх-вниз по раме
   6. Случайные вспышки отдельных рёбер
   7. Случайные вспышки отдельных диодов
   8. Диодные фигуры (2 точки / 1 точка на ребро, по очереди)

  Мастер-регуляторы (действуют на ВСЁ сразу, из браузера):
   - Яркость (0-255)
   - Скорость (0.3x - 3.0x) — множитель на все паузы/длительности

  ВАЖНО ПЕРЕД ЗАЛИВКОЙ: впишите WIFI_SSID / WIFI_PASSWORD ниже.

  ПОСЛЕ ПЕРВОГО ЗАПУСКА ПРОВЕРЬТЕ:
   - Эффект 1 "бегущая линия": если движение по какому-то ребру
     выглядит "задом наперёд" — переключите соответствующий флаг
     в массиве EDGE_REV ниже (true/false) и перезалейте.
   - Эффект 5, фаза "квадраты": я объединил ваше "справа налево"
     и "вверх/вниз" так — марш по рядам вверх, в каждом ряду сперва
     правый квадрат (M-R), потом левый (L-M); затем то же вниз.
     Если задумывалось иначе — легко поправить в prog5_march().
*/

#include <WiFi.h>
#include <WebServer.h>
#include <FastLED.h>

// ====== WiFi ======
const char* WIFI_SSID     = "Chigreculturallatadezinc";
const char* WIFI_PASSWORD = "Latadezinc1312";

// ====== Выходы (подтверждённая физическая раскладка) ======
#define PIN_BUS0 16
#define PIN_BUS1 3
#define PIN_BUS2 1
#define PIN_BUS3 4
#define LEN_BUS0 360
#define LEN_BUS1 432
#define LEN_BUS2 360
#define LEN_BUS3 432
#define TOTAL_LEDS (LEN_BUS0+LEN_BUS1+LEN_BUS2+LEN_BUS3)

CRGB leds0[LEN_BUS0];
CRGB leds1[LEN_BUS1];
CRGB leds2[LEN_BUS2];
CRGB leds3[LEN_BUS3];

// Этот тип нужен для программы 1 (бегущая линия). Объявлен здесь,
// в самом начале файла, — чтобы компилятор точно знал о нём заранее.
struct PathStep { uint8_t edge; bool rev; };

WebServer server(80);

// ====== Откалиброванные границы 22 рёбер ======
const uint16_t edgeStart[22] = {1004,640,934,570,709,864,501,1080,793,431,1152,360,1222,0,288,1504,72,1293,1434,141,1364,211};
const uint16_t edgeStop[22]  = {1076,709,1004,640,780,934,570,1152,864,501,1222,431,1293,72,360,1575,141,1364,1504,211,1434,281};

// Если после теста эффект 1 идёт по какому-то ребру "задом наперёд" — смените true/false тут
bool EDGE_REV[22] = {false,false,false,false,false,false,false,false,false,false,
                      false,false,false,false,false,false,false,false,false,false,false,false};

// ====== Сетка конструкции ======
const uint8_t rowH[5][2] = {{1,2},{6,7},{11,12},{16,17},{21,22}};
const uint8_t colL[4] = {3,8,13,18};
const uint8_t colM[4] = {4,9,14,19};
const uint8_t colR[4] = {5,10,15,20};

// ====== Мастер-регуляторы ======
uint8_t masterBrightness = 255;
float speedFactor = 1.0;
unsigned long T(unsigned long ms) { return (unsigned long)(ms / speedFactor); }

// ====== Низкоуровневые функции рисования ======
void setGlobal(int idx, CRGB c) {
  if (idx < 0 || idx >= TOTAL_LEDS) return;
  if (idx < LEN_BUS0) { leds0[idx] = c; return; }
  idx -= LEN_BUS0;
  if (idx < LEN_BUS1) { leds1[idx] = c; return; }
  idx -= LEN_BUS1;
  if (idx < LEN_BUS2) { leds2[idx] = c; return; }
  idx -= LEN_BUS2;
  if (idx < LEN_BUS3) { leds3[idx] = c; }
}
void edgeSet(int edgeNum, CRGB c) {
  int i = edgeNum - 1;
  for (int p = edgeStart[i]; p < edgeStop[i]; p++) setGlobal(p, c);
}
void edgesSet(const uint8_t* edges, int len, CRGB c) {
  for (int k = 0; k < len; k++) edgeSet(edges[k], c);
}
void clearAll() {
  fill_solid(leds0, LEN_BUS0, CRGB::Black);
  fill_solid(leds1, LEN_BUS1, CRGB::Black);
  fill_solid(leds2, LEN_BUS2, CRGB::Black);
  fill_solid(leds3, LEN_BUS3, CRGB::Black);
}

// ====== Генератор прямоугольника/квадрата по сетке ======
// colMode: 0=L-M, 1=M-R, 2=L-R(вся ширина). rowStart..rowEnd: 0..3
void buildRect(int colMode, int rowStart, int rowEnd, uint8_t* out, uint8_t &outLen) {
  outLen = 0;
  if (colMode == 0) {
    out[outLen++] = rowH[rowStart][0];
    out[outLen++] = rowH[rowEnd+1][0];
    for (int r = rowStart; r <= rowEnd; r++) { out[outLen++] = colL[r]; out[outLen++] = colM[r]; }
  } else if (colMode == 1) {
    out[outLen++] = rowH[rowStart][1];
    out[outLen++] = rowH[rowEnd+1][1];
    for (int r = rowStart; r <= rowEnd; r++) { out[outLen++] = colM[r]; out[outLen++] = colR[r]; }
  } else {
    out[outLen++] = rowH[rowStart][0]; out[outLen++] = rowH[rowStart][1];
    out[outLen++] = rowH[rowEnd+1][0]; out[outLen++] = rowH[rowEnd+1][1];
    for (int r = rowStart; r <= rowEnd; r++) { out[outLen++] = colL[r]; out[outLen++] = colR[r]; }
  }
}

// ====== Пул "ломаных" фигур для программы 4 (0 = конец списка) ======
const uint8_t brokenShapes[11][5] = {
  {9,12,15,17,16}, {18,16,14,0,0}, {1,4,6,0,0}, {2,5,7,0,0}, {3,6,9,11,0},
  {13,16,19,0,0}, {2,5,10,0,0}, {11,14,17,20,0}, {8,11,12,0,0}, {7,10,12,0,0}, {16,19,21,0,0}
};
const uint8_t brokenLen[11] = {5,3,3,3,4,3,3,4,3,3,3};

// ====================================================================
// ПРОГРАММЫ
// ====================================================================
enum Prog { P_OFF, P1_LINE, P2_PAIRS, P3_RECTS, P4_BROKEN, P5_MARCH, P6_EDGES, P7_DOTS, P8_DOTSHAPE };
Prog currentProg = P_OFF;
unsigned long nextT = 0;
int state = 0, subState = 0, repeatCnt = 0;
unsigned long phaseStartMs = 0;

void resetProgState() { state = 0; subState = 0; repeatCnt = 0; nextT = 0; phaseStartMs = millis(); }

// ---------- Программа 1: бегущая линия ----------
void getColumnPath(int col, bool bottomToTop, PathStep* out) {
  const uint8_t* c = (col==0)?colL:(col==1)?colM:colR;
  for (int k=0;k<4;k++){
    int r = bottomToTop ? (3-k) : k;
    bool physRev = EDGE_REV[c[r]-1];
    out[k].edge = c[r];
    out[k].rev = bottomToTop ? !physRev : physRev;
  }
}

void drawMovingWindow(PathStep* path, int steps, float frac) {
  int total = 0, lens[12];
  for (int i=0;i<steps;i++){ lens[i] = edgeStop[path[i].edge-1]-edgeStart[path[i].edge-1]; total += lens[i]; }
  int halfLen = lens[0]/2; if (halfLen<3) halfLen=3;
  int pos = (int)(frac * (total - halfLen));
  int acc = 0;
  for (int i=0;i<steps;i++){
    int segStart = acc, segEnd = acc+lens[i];
    for (int w=pos; w<pos+halfLen; w++){
      if (w>=segStart && w<segEnd){
        int local = w-segStart;
        int gp = path[i].rev ? (edgeStop[path[i].edge-1]-1-local) : (edgeStart[path[i].edge-1]+local);
        setGlobal(gp, CRGB::White);
      }
    }
    acc += lens[i];
  }
}

void prog1_line() {
  unsigned long now = millis();
  static PathStep path1[12]; static PathStep pathC[3][4];

  if (subState == 0) {
    if (state==0 || state==2) {
      bool up = (state==0);
      PathStep tmp[4];
      getColumnPath(0, up, tmp); for(int i=0;i<4;i++) path1[i]=tmp[i];
      getColumnPath(1, up, tmp); for(int i=0;i<4;i++) path1[4+i]=tmp[i];
      getColumnPath(2, up, tmp); for(int i=0;i<4;i++) path1[8+i]=tmp[i];
    } else {
      bool up = (state==1);
      getColumnPath(0, up, pathC[0]);
      getColumnPath(1, up, pathC[1]);
      getColumnPath(2, up, pathC[2]);
    }
    phaseStartMs = now;
    subState = 1;
  }

  unsigned long dur = (state==0||state==2) ? T(3000) : T(1000);

  if (subState == 1) {
    unsigned long elapsed = now - phaseStartMs; if (elapsed>dur) elapsed=dur;
    float frac = (float)elapsed/dur;
    clearAll();
    if (state==0 || state==2) {
      drawMovingWindow(path1, 12, frac);
    } else {
      for (int c=0;c<3;c++) drawMovingWindow(pathC[c], 4, frac);
    }
    if (now - phaseStartMs >= dur) { subState = 2; nextT = now + T(5000); clearAll(); }
  } else if (subState == 2) {
    if (now >= nextT) {
      repeatCnt++;
      if (repeatCnt >= 3) { repeatCnt = 0; state = (state+1)%4; }
      subState = 0;
    }
  }
}

// ---------- Программа 2: пары рёбер снизу вверх ----------
const uint8_t pairSeq[5][2] = {{21,22},{16,17},{11,12},{6,7},{1,2}};
void prog2_pairs() {
  unsigned long now = millis();
  if (subState == 0) { clearAll(); edgesSet(pairSeq[state], 2, CRGB::White); nextT = now + T(500); subState = 1; }
  else if (subState == 1 && now >= nextT) { clearAll(); nextT = now + T(3000); subState = 2; }
  else if (subState == 2 && now >= nextT) { state = (state+1)%5; subState = 0; }
}

// ---------- Программа 3: случайные квадраты/прямоугольники ----------
void prog3_rects() {
  unsigned long now = millis();
  if (subState == 0) {
    uint8_t buf[12], len;
    int colMode = random(0,3);
    int rowStart = random(0,4);
    int rowEnd = rowStart + random(0, 4-rowStart);
    buildRect(colMode, rowStart, rowEnd, buf, len);
    clearAll();
    edgesSet(buf, len, CRGB::White);
    nextT = now + T(500);
    subState = 1;
  } else if (subState == 1 && now >= nextT) {
    clearAll(); nextT = now + T(random(1000,3000)); subState = 2;
  } else if (subState == 2 && now >= nextT) {
    subState = 0;
  }
}

// ---------- Программа 4: ломаные фигуры ----------
void prog4_broken() {
  unsigned long now = millis();
  if (subState == 0) {
    int idx = random(0,11);
    clearAll();
    edgesSet(brokenShapes[idx], brokenLen[idx], CRGB::White);
    nextT = now + T(500);
    subState = 1;
  } else if (subState == 1 && now >= nextT) {
    clearAll(); nextT = now + T(random(1000,3000)); subState = 2;
  } else if (subState == 2 && now >= nextT) {
    subState = 0;
  }
}

// ---------- Программа 5: марш прямоугольников/квадратов ----------
const int marchOrder[8] = {3,2,1,0,0,1,2,3};
void prog5_march() {
  unsigned long now = millis();
  static int lastKey = -999;
  static int rl = 0; // 0=право(MR) 1=лево(LM), для фазы B
  static int mini = 0; // 0=показ, 1=пауза

  int band = marchOrder[subState];
  int key = state*100 + subState*10 + rl;

  if (mini == 0 && key != lastKey) {
    uint8_t buf[12], len;
    if (state == 0) buildRect(2, band, band, buf, len);
    else buildRect(rl==0?1:0, band, band, buf, len);
    clearAll();
    edgesSet(buf, len, CRGB::White);
    nextT = now + T(500);
    lastKey = key;
  }

  if (mini == 0 && now >= nextT) { clearAll(); nextT = now + T(3000); mini = 1; }
  else if (mini == 1 && now >= nextT) {
    mini = 0;
    if (state == 0) {
      subState++;
      if (subState >= 8) { subState = 0; state = 1; }
    } else {
      rl++;
      if (rl >= 2) {
        rl = 0; subState++;
        if (subState >= 8) { subState = 0; state = 0; }
      }
    }
    lastKey = -999;
  }
}

// ---------- Программа 6: случайные рёбра ----------
void prog6_edges() {
  unsigned long now = millis();
  if (subState == 0) {
    clearAll(); edgeSet(random(1,23), CRGB::White);
    nextT = now + T(500); subState = 1;
  } else if (subState == 1 && now >= nextT) {
    clearAll(); nextT = now + T(2000); subState = 2;
  } else if (subState == 2 && now >= nextT) {
    subState = 0;
  }
}

// ---------- Программа 7: случайные диоды ----------
void prog7_dots() {
  unsigned long now = millis();
  if (subState == 0) {
    clearAll(); setGlobal(random(0, TOTAL_LEDS), CRGB::White);
    nextT = now + T(500); subState = 1;
  } else if (subState == 1 && now >= nextT) {
    clearAll(); nextT = now + T(random(1000,3000)); subState = 2;
  } else if (subState == 2 && now >= nextT) {
    subState = 0;
  }
}

// ---------- Программа 8: диодные фигуры ----------
void prog8_dotshape() {
  unsigned long now = millis();
  if (subState == 0) {
    clearAll();
    for (int e=1;e<=22;e++){
      int L = edgeStop[e-1]-edgeStart[e-1];
      if (state==0) {
        setGlobal(edgeStart[e-1] + L/3, CRGB::White);
        setGlobal(edgeStart[e-1] + (2*L)/3, CRGB::White);
      } else {
        setGlobal(edgeStart[e-1] + L/2, CRGB::White);
      }
    }
    nextT = now + T(500);
    subState = 1;
  } else if (subState == 1 && now >= nextT) {
    clearAll(); nextT = now + T(3000); subState = 2;
  } else if (subState == 2 && now >= nextT) {
    repeatCnt++;
    if (repeatCnt >= 3) { repeatCnt = 0; state = 1-state; }
    subState = 0;
  }
}

void runCurrentProgram() {
  switch (currentProg) {
    case P1_LINE: prog1_line(); break;
    case P2_PAIRS: prog2_pairs(); break;
    case P3_RECTS: prog3_rects(); break;
    case P4_BROKEN: prog4_broken(); break;
    case P5_MARCH: prog5_march(); break;
    case P6_EDGES: prog6_edges(); break;
    case P7_DOTS: prog7_dots(); break;
    case P8_DOTSHAPE: prog8_dotshape(); break;
    default: clearAll(); break;
  }
}

// ====================================================================
// Веб-интерфейс
// ====================================================================
const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Свет</title>
<style>
body{font-family:sans-serif;background:#111;color:#eee;text-align:center;padding:20px}
button{font-size:17px;padding:14px 8px;margin:5px;border-radius:10px;border:none;background:#333;color:#fff;width:90%;max-width:320px}
input[type=range]{width:90%;max-width:320px}
h3{margin-top:28px}
</style></head><body>
<h2>Управление светом</h2>

<h3>Яркость</h3>
<input type="range" min="0" max="255" value="255" oninput="fetch('/set?bri='+this.value)">

<h3>Скорость</h3>
<input type="range" min="30" max="300" value="100" oninput="fetch('/set?spd='+this.value)">

<h3>Программы</h3>
<button onclick="prog(0)">Выключить</button>
<button onclick="prog(1)">1. Бегущая линия</button>
<button onclick="prog(2)">2. Пары рёбер</button>
<button onclick="prog(3)">3. Квадраты/прямоугольники</button>
<button onclick="prog(4)">4. Ломаные фигуры</button>
<button onclick="prog(5)">5. Марш фигур</button>
<button onclick="prog(6)">6. Случайные рёбра</button>
<button onclick="prog(7)">7. Случайные диоды</button>
<button onclick="prog(8)">8. Диодные фигуры</button>

<script>
function prog(n){ fetch('/prog?n='+n); }
</script>
</body></html>
)HTML";

void handleRoot() { server.send(200, "text/html", PAGE); }

void handleProg() {
  int n = server.arg("n").toInt();
  currentProg = (Prog)n;
  resetProgState();
  server.send(200, "text/plain", "ok");
}

void handleSet() {
  if (server.hasArg("bri")) {
    masterBrightness = server.arg("bri").toInt();
    FastLED.setBrightness(masterBrightness);
  }
  if (server.hasArg("spd")) {
    speedFactor = server.arg("spd").toInt() / 100.0;
  }
  server.send(200, "text/plain", "ok");
}

void setup() {
  Serial.begin(115200);

  FastLED.addLeds<WS2812B, PIN_BUS0, GRB>(leds0, LEN_BUS0);
  FastLED.addLeds<WS2812B, PIN_BUS1, GRB>(leds1, LEN_BUS1);
  FastLED.addLeds<WS2812B, PIN_BUS2, GRB>(leds2, LEN_BUS2);
  FastLED.addLeds<WS2812B, PIN_BUS3, GRB>(leds3, LEN_BUS3);
  FastLED.setBrightness(masterBrightness);
  clearAll();
  FastLED.show();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.println();
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/prog", handleProg);
  server.on("/set", handleSet);
  server.begin();

  randomSeed(analogRead(36));
  resetProgState();
}

void loop() {
  server.handleClient();
  runCurrentProgram();
  FastLED.show();
}
