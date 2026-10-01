/*
  QuinLED Dig-Quad — 13 световых программ + веб-управление (цвет/яркость/скорость)
  ================================================================================
  Программы (кнопки в браузере):
   1. Бегущая линия (резко)
   2. Удаление пар (резко)
   3. Вспышки фигур (резко, двойная вспышка)
   4. Вспышки ломаных фигур (резко, двойная вспышка)
   5. Движение прямоугольниками и квадратами (резко + плавное угасание)
   6. Случайные вспышки линий (тройная вспышка)
   7. Случайные вспышки отдельных диодов (плавно, несколько одновременно)
   8. Вспышки диодных фигур (плавно, 2 диода на ребро, все рёбра разом)
   9. Вспышки диодов постоянные (плавно, один за другим без пауз)
   10. Движение одного диода (медленно, по соединённым рёбрам, как в WLED)
   11. Медленные линии туда и обратно (плавно)
   12. Стробоскоп (вся рама)
   13. Стробоскоп периметр (только внешний контур)

  Мастер-регуляторы на странице (действуют на ВСЁ сразу):
   - Яркость: 0 (выкл) .. 255 (максимум, как в первой версии)
   - Скорость: ползунок 0-100. В середине (50) = тайминги "по умолчанию"
     из ТЗ. Влево до 0 — почти полная остановка движения (~x0.05).
     Вправо до 100 — ускорение максимум в 1.5 раза.
   - Цвет: круглый цветовой регулятор (колесо), как в WLED. По умолчанию
     после включения — красный.

  ВАЖНО ПЕРЕД ЗАЛИВКОЙ: WIFI_SSID / WIFI_PASSWORD уже вписаны ниже
  (те же, что вы использовали в прошлой версии). Проверьте, если сеть
  сменилась.

  ПОСЛЕ ПЕРВОГО ЗАПУСКА ПРОВЕРЬТЕ (я делал разумные предположения там,
  где в ТЗ было неоднозначно — если что-то выглядит не так, скажите мне
  какой пункт и как должно быть, поправим):
   - Программа 1: если движение по какому-то ребру идёт "задом наперёд" —
     переключите нужный флаг в EDGE_REV (true/false) и перезалейте.
   - Программа 5: я скомпоновал фазу 1 как маршевое движение
     полноширинных прямоугольников по рядам вверх/вниз, а фазу 2 —
     как маленькие квадраты, идущие справа-налево на каждом ряду,
     тоже вверх/вниз.
   - Программа 10: диод "путешествует" по случайному связному маршруту
     рёбер (граф соединений собран автоматически по сетке). Если в
     какой-то момент направление внутри ребра будет визуально
     "перевёрнутым" относительно физической стороны — это не критично
     для эффекта, но можно поправить через EDGE_REV при желании.
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

// ====== Типы (объявлены в самом начале, до любых функций) ======
struct PathStep { uint8_t edge; bool rev; };
struct DotSlot { int idx; uint8_t phase; unsigned long t0; unsigned long waitMs; };

WebServer server(80);

// ====== Откалиброванные границы 22 рёбер ======
const uint16_t edgeStart[22] = {1004,640,934,570,709,864,501,1076,793,431,1152,360,1222,0,282,1504,72,1293,1434,141,1364,211};
const uint16_t edgeStop[22]  = {1075,709,1004,640,780,934,570,1152,864,501,1222,431,1293,72,360,1575,141,1364,1504,211,1434,281};

bool EDGE_REV[22] = {false,false,false,false,false,false,false,false,false,false,
                      false,false,false,false,false,false,false,false,false,false,false,false};

// ====== Сетка конструкции ======
// rowH[row] = {левое ребро ряда, правое ребро ряда}, row 0..4 сверху вниз
const uint8_t rowH[5][2] = {{1,2},{6,7},{11,12},{16,17},{21,22}};
// colL/colM/colR[row] = вертикальное ребро между рядом row и row+1, row 0..3
const uint8_t colL[4] = {3,8,13,18};
const uint8_t colM[4] = {4,9,14,19};
const uint8_t colR[4] = {5,10,15,20};

// Периметр рамы (для программы 13)
const uint8_t perimeterEdges[12] = {1,2,21,22,3,8,13,18,5,10,15,20};

// ====== Мастер-регуляторы ======
uint8_t masterBrightness = 255;
float speedFactor = 1.0;
CRGB currentColor = CRGB::Red;

unsigned long T(unsigned long ms) { return (unsigned long)(ms / speedFactor); }

float mapSpeed(int v) { // v: 0..100, 50 = базовая скорость (x1.0)
  if (v < 0) v = 0; if (v > 100) v = 100;
  if (v <= 50) return 0.05f + (1.0f - 0.05f) * (v / 50.0f);
  return 1.0f + (1.5f - 1.0f) * ((v - 50) / 50.0f);
}

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
CRGB scaledColor(uint8_t scale) {
  CRGB c = currentColor;
  c.nscale8_video(scale);
  return c;
}
// Рисует только часть ребра: fracStart..fracStart+fracLen (0.0-1.0 от длины ребра)
void drawPartialEdge(int edgeNum, float fracStart, float fracLen, CRGB c) {
  int i = edgeNum - 1;
  int len = edgeStop[i] - edgeStart[i];
  int a = edgeStart[i] + (int)(fracStart * len);
  int b = a + (int)(fracLen * len);
  if (b > edgeStop[i]) b = edgeStop[i];
  for (int p = a; p < b; p++) setGlobal(p, c);
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

// Рисует ребро "рвано" — со случайным пробелом внутри (для ломаных фигур)
void drawJaggedEdge(int edgeNum, CRGB c) {
  float startA = random(0, 30) / 100.0;         // 0-30%
  float lenA = random(20, 40) / 100.0;          // 20-40%
  drawPartialEdge(edgeNum, startA, lenA, c);
  float startB = startA + lenA + random(10, 25) / 100.0; // пробел 10-25%
  float lenB = random(20, 40) / 100.0;
  if (startB + lenB > 1.0) lenB = 1.0 - startB;
  if (lenB > 0.05) drawPartialEdge(edgeNum, startB, lenB, c);
}

// ====== Граф соединений рёбер (для программы 10 — движение диода) ======
// Узлы сетки: node = row*3 + col (row 0..4, col 0=L,1=M,2=R) => 0..14
uint8_t edgeNodeA[23], edgeNodeB[23]; // индекс = номер ребра (1..22)
uint8_t adjEdges[15][4];
uint8_t adjDegree[15];

int nodeIdx(int row, int col) { return row * 3 + col; }

void addAdj(int node, int edgeNum) {
  if (adjDegree[node] < 4) { adjEdges[node][adjDegree[node]] = edgeNum; adjDegree[node]++; }
}

void buildGraph() {
  for (int n = 0; n < 15; n++) adjDegree[n] = 0;
  for (int r = 0; r < 5; r++) {
    int a = nodeIdx(r, 0), m = nodeIdx(r, 1), b = nodeIdx(r, 2);
    edgeNodeA[rowH[r][0]] = a; edgeNodeB[rowH[r][0]] = m; addAdj(a, rowH[r][0]); addAdj(m, rowH[r][0]);
    edgeNodeA[rowH[r][1]] = m; edgeNodeB[rowH[r][1]] = b; addAdj(m, rowH[r][1]); addAdj(b, rowH[r][1]);
  }
  for (int r = 0; r < 4; r++) {
    int topL = nodeIdx(r,0), botL = nodeIdx(r+1,0);
    int topM = nodeIdx(r,1), botM = nodeIdx(r+1,1);
    int topR = nodeIdx(r,2), botR = nodeIdx(r+1,2);
    edgeNodeA[colL[r]] = topL; edgeNodeB[colL[r]] = botL; addAdj(topL, colL[r]); addAdj(botL, colL[r]);
    edgeNodeA[colM[r]] = topM; edgeNodeB[colM[r]] = botM; addAdj(topM, colM[r]); addAdj(botM, colM[r]);
    edgeNodeA[colR[r]] = topR; edgeNodeB[colR[r]] = botR; addAdj(topR, colR[r]); addAdj(botR, colR[r]);
  }
}

// ====================================================================
// ПРОГРАММЫ
// ====================================================================
enum Prog { P_OFF, P1_LINE, P2_PAIRS, P3_RECTS, P4_BROKEN, P5_MARCH, P6_LINES,
            P7_DOTS, P8_DOTSHAPE, P9_DOTS_CONT, P10_MOVE_DOT, P11_SLOWPAIRS,
            P12_STROBE, P13_STROBE_PERI };
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
        setGlobal(gp, currentColor);
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

  unsigned long dur = (state==0||state==2) ? T(5000) : T(1000);

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

// ---------- Программа 2: удаление пар (резко) ----------
const uint8_t pairSeq[5][2] = {{21,22},{16,17},{11,12},{6,7},{1,2}};
void prog2_pairs() {
  unsigned long now = millis();
  if (subState == 0) { clearAll(); edgesSet(pairSeq[state], 2, currentColor); nextT = now + T(200); subState = 1; }
  else if (subState == 1 && now >= nextT) {
    clearAll();
    state++;
    if (state >= 5) { state = 0; nextT = now + T(6000); subState = 2; }
    else { subState = 0; }
  } else if (subState == 2 && now >= nextT) { subState = 0; }
}

// ---------- Программа 3: случайные вспышки фигур (двойная вспышка) ----------
void prog3_rects() {
  unsigned long now = millis();
  static uint8_t buf[12]; static uint8_t len;
  if (subState == 0) {
    int colMode = random(0,3);
    int rowStart = random(0,4);
    int rowEnd = rowStart + random(0, 4-rowStart);
    buildRect(colMode, rowStart, rowEnd, buf, len);
    clearAll(); edgesSet(buf, len, currentColor);
    nextT = now + T(50); subState = 1;
  } else if (subState == 1 && now >= nextT) { clearAll(); nextT = now + T(50); subState = 2; }
  else if (subState == 2 && now >= nextT) { edgesSet(buf, len, currentColor); nextT = now + T(50); subState = 3; }
  else if (subState == 3 && now >= nextT) { clearAll(); nextT = now + T(6000); subState = 4; }
  else if (subState == 4 && now >= nextT) { subState = 0; }
}

// ---------- Программа 4: ломаные фигуры (двойная вспышка, с пробелами) ----------
void prog4_broken() {
  unsigned long now = millis();
  static int idx;
  if (subState == 0) {
    idx = random(0,11);
    clearAll();
    for (int k=0;k<brokenLen[idx];k++) drawJaggedEdge(brokenShapes[idx][k], currentColor);
    nextT = now + T(50); subState = 1;
  } else if (subState == 1 && now >= nextT) { clearAll(); nextT = now + T(50); subState = 2; }
  else if (subState == 2 && now >= nextT) {
    for (int k=0;k<brokenLen[idx];k++) drawJaggedEdge(brokenShapes[idx][k], currentColor);
    nextT = now + T(50); subState = 3;
  } else if (subState == 3 && now >= nextT) { clearAll(); nextT = now + T(6000); subState = 4; }
  else if (subState == 4 && now >= nextT) { subState = 0; }
}

// ---------- Программа 5: марш прямоугольников/квадратов ----------
void prog5_march() {
  unsigned long now = millis();
  static uint8_t buf[12]; static uint8_t len;
  static int row = 0, dir = 1; // dir 1=вниз(row++), -1=вверх(row--)
  static int rl = 0; // для квадратов: 0=справа(M-R), 1=слева(L-M)

  if (subState == 0) {
    if (state == 0) buildRect(2, row, row, buf, len);           // фаза1: прямоугольники во всю ширину
    else buildRect(rl==0?1:0, row, row, buf, len);               // фаза2: маленькие квадраты, справа->слева
    clearAll(); edgesSet(buf, len, currentColor);
    phaseStartMs = now; nextT = now + T(200); subState = 1;
  } else if (subState == 1 && now >= nextT) { subState = 2; phaseStartMs = now; }
  else if (subState == 2) {
    unsigned long dur = T(1000);
    unsigned long elapsed = now - phaseStartMs; if (elapsed>dur) elapsed=dur;
    uint8_t scale = 255 - (uint8_t)(255.0 * elapsed / dur);
    clearAll(); edgesSet(buf, len, scaledColor(scale));
    if (elapsed >= dur) { clearAll(); nextT = now + T(6000); subState = 3; }
  } else if (subState == 3 && now >= nextT) {
    if (state == 0) {
      row += dir;
      if (row > 3) { row = 3; dir = -1; row += dir; }
      else if (row < 0) { row = 0; dir = 1; state = 1; row = 0; dir = 1; rl = 0; }
    } else {
      rl = 1 - rl;
      if (rl == 0) {
        row += dir;
        if (row > 3) { row = 3; dir = -1; row += dir; }
        else if (row < 0) { row = 0; dir = 1; state = 0; row = 0; }
      }
    }
    subState = 0;
  }
}

// ---------- Программа 6: случайные вспышки линий (тройная вспышка) ----------
void prog6_lines() {
  unsigned long now = millis();
  static uint8_t buf[3]; static uint8_t len; static bool partial; static float pStart;
  if (subState == 0) {
    int mode = random(0,3); // 0=одиночное частичное ребро, 1=цепочка колонны, 2=пара ряда
    if (mode == 0) {
      const uint8_t* col = (random(0,3)==0)?colL:(random(0,2)==0?colM:colR);
      buf[0] = col[random(0,4)]; len = 1; partial = true;
      pStart = random(0,50)/100.0;
    } else if (mode == 2) {
      int r = random(0,5); buf[0]=rowH[r][0]; buf[1]=rowH[r][1]; len=2; partial=false;
    } else {
      const uint8_t* col = (random(0,3)==0)?colL:(random(0,2)==0?colM:colR);
      int chainLen = random(2,4); // 2 или 3
      int s = random(0,4-chainLen+1);
      for (int k=0;k<chainLen;k++) buf[k]=col[s+k];
      len = chainLen; partial=false;
    }
    nextT = now + T(200); subState = 1;
  }
  else if (subState == 1) { clearAll(); if(partial) drawPartialEdge(buf[0],pStart,0.4,currentColor); else edgesSet(buf,len,currentColor); if(now>=nextT){nextT=now+T(80); subState=2;} }
  else if (subState == 2 && now >= nextT) { clearAll(); nextT = now + T(200); subState = 3; }
  else if (subState == 3) { if(partial) drawPartialEdge(buf[0],pStart,0.4,currentColor); else edgesSet(buf,len,currentColor); if(now>=nextT){nextT=now+T(80); subState=4;} }
  else if (subState == 4 && now >= nextT) { clearAll(); nextT = now + T(200); subState = 5; phaseStartMs = now; }
  else if (subState == 5) {
    unsigned long dur = T(700);
    unsigned long elapsed = now - phaseStartMs; if (elapsed>dur) elapsed=dur;
    uint8_t scale = 255 - (uint8_t)(255.0 * elapsed / dur);
    clearAll();
    if(partial) drawPartialEdge(buf[0],pStart,0.4,scaledColor(scale)); else edgesSet(buf,len,scaledColor(scale));
    if (elapsed >= dur) { clearAll(); nextT = now + T(6000); subState = 6; }
  }
  else if (subState == 6 && now >= nextT) { subState = 0; }
}

// ---------- Программа 7: случайные вспышки отдельных диодов (пул) ----------
#define DOT_POOL 15
DotSlot dotPool[DOT_POOL];
bool dotPoolInit = false;

void initDotPool() {
  for (int i=0;i<DOT_POOL;i++) {
    dotPool[i].idx = random(0, TOTAL_LEDS);
    dotPool[i].phase = 0;
    dotPool[i].t0 = millis();
    dotPool[i].waitMs = random(1000,10000);
  }
  dotPoolInit = true;
}

void prog7_dots() {
  if (!dotPoolInit) initDotPool();
  unsigned long now = millis();
  clearAll();
  for (int i=0;i<DOT_POOL;i++) {
    DotSlot &s = dotPool[i];
    unsigned long elapsed = now - s.t0;
    if (s.phase == 0) { // ожидание
      if (elapsed >= T(s.waitMs)) { s.phase=1; s.t0=now; s.idx = random(0,TOTAL_LEDS); }
    } else if (s.phase == 1) { // разгорание 0.8s
      unsigned long dur = T(800);
      float frac = (float)elapsed/dur; if(frac>1) frac=1;
      setGlobal(s.idx, scaledColor((uint8_t)(255*frac)));
      if (elapsed >= dur) { s.phase=2; s.t0=now; }
    } else if (s.phase == 2) { // угасание 0.8s
      unsigned long dur = T(800);
      float frac = (float)elapsed/dur; if(frac>1) frac=1;
      setGlobal(s.idx, scaledColor((uint8_t)(255*(1-frac))));
      if (elapsed >= dur) { s.phase=0; s.t0=now; s.waitMs=random(1000,10000); }
    }
  }
}

// ---------- Программа 8: вспышки диодных фигур (2 точки на ребро, все рёбра) ----------
void prog8_dotshape() {
  unsigned long now = millis();
  if (subState == 0) { phaseStartMs = now; subState = 1; }
  if (subState == 1) {
    unsigned long dur = T(800);
    unsigned long elapsed = now - phaseStartMs; if (elapsed>dur) elapsed=dur;
    uint8_t scale = (uint8_t)(255.0*elapsed/dur);
    clearAll();
    for (int e=1;e<=22;e++){
      int L = edgeStop[e-1]-edgeStart[e-1];
      setGlobal(edgeStart[e-1] + L/3, scaledColor(scale));
      setGlobal(edgeStart[e-1] + (2*L)/3, scaledColor(scale));
    }
    if (elapsed >= dur) { subState = 2; phaseStartMs = now; }
  } else if (subState == 2) {
    unsigned long dur = T(800);
    unsigned long elapsed = now - phaseStartMs; if (elapsed>dur) elapsed=dur;
    uint8_t scale = 255 - (uint8_t)(255.0*elapsed/dur);
    clearAll();
    for (int e=1;e<=22;e++){
      int L = edgeStop[e-1]-edgeStart[e-1];
      setGlobal(edgeStart[e-1] + L/3, scaledColor(scale));
      setGlobal(edgeStart[e-1] + (2*L)/3, scaledColor(scale));
    }
    if (elapsed >= dur) { clearAll(); nextT = now + T(6000); subState = 3; }
  } else if (subState == 3 && now >= nextT) { subState = 0; }
}

// ---------- Программа 9: вспышки диодов постоянные (без пауз) ----------
void prog9_dots_cont() {
  unsigned long now = millis();
  static int dotIdx; static uint8_t br;
  if (subState == 0) { dotIdx = random(0,TOTAL_LEDS); br = random(120,255); phaseStartMs = now; subState = 1; }
  if (subState == 1) {
    unsigned long dur = T(400);
    unsigned long elapsed = now - phaseStartMs; if (elapsed>dur) elapsed=dur;
    float frac = (float)elapsed/dur;
    clearAll(); setGlobal(dotIdx, scaledColor((uint8_t)(br*frac)));
    if (elapsed >= dur) { subState = 2; phaseStartMs = now; }
  } else if (subState == 2) {
    unsigned long dur = T(400);
    unsigned long elapsed = now - phaseStartMs; if (elapsed>dur) elapsed=dur;
    float frac = (float)elapsed/dur;
    clearAll(); setGlobal(dotIdx, scaledColor((uint8_t)(br*(1-frac))));
    if (elapsed >= dur) { subState = 0; } // сразу следующий диод, без паузы
  }
}

// ---------- Программа 10: движение одного диода по графу рёбер ----------
void prog10_move_dot() {
  unsigned long now = millis();
  static int curEdge = -1, fromNode = -1, toNode = -1;
  static uint8_t curBrightness = 255;

  if (curEdge == -1) {
    curEdge = 1; fromNode = edgeNodeA[curEdge]; toNode = edgeNodeB[curEdge];
    phaseStartMs = now; curBrightness = random(140,255);
  }

  unsigned long dur = T(2000);
  unsigned long elapsed = now - phaseStartMs; if (elapsed>dur) elapsed=dur;
  float frac = (float)elapsed/dur;

  int i = curEdge - 1;
  int len = edgeStop[i]-edgeStart[i];
  bool goForward = (fromNode == edgeNodeA[curEdge]); // fromNode=A -> двигаться A->B (start->stop)
  int pos = goForward ? (int)(frac*len) : (int)((1-frac)*len);
  clearAll();
  setGlobal(edgeStart[i]+pos, scaledColor(curBrightness));

  if (elapsed >= dur) {
    int node = toNode;
    int deg = adjDegree[node];
    if (deg > 0) {
      int pick;
      int tries = 0;
      do { pick = adjEdges[node][random(0,deg)]; tries++; } while (pick == curEdge && deg>1 && tries<5);
      int newFrom = node;
      int newTo = (edgeNodeA[pick]==node) ? edgeNodeB[pick] : edgeNodeA[pick];
      curEdge = pick; fromNode = newFrom; toNode = newTo;
    }
    phaseStartMs = now; curBrightness = random(140,255);
  }
}

// ---------- Программа 11: медленные линии туда и обратно ----------
const int slowSeq[9] = {0,1,2,3,4,3,2,1,0}; // индексы в pairSeq, вперёд и назад
void prog11_slowpairs() {
  unsigned long now = millis();
  if (subState == 0) { phaseStartMs = now; subState = 1; }
  if (subState == 1) {
    unsigned long dur = T(800);
    unsigned long elapsed = now - phaseStartMs; if (elapsed>dur) elapsed=dur;
    float frac = (float)elapsed/dur;
    clearAll(); edgesSet(pairSeq[slowSeq[state]], 2, scaledColor((uint8_t)(255*frac)));
    if (elapsed >= dur) { subState = 2; phaseStartMs = now; }
  } else if (subState == 2) {
    unsigned long dur = T(800);
    unsigned long elapsed = now - phaseStartMs; if (elapsed>dur) elapsed=dur;
    float frac = (float)elapsed/dur;
    clearAll(); edgesSet(pairSeq[slowSeq[state]], 2, scaledColor((uint8_t)(255*(1-frac))));
    if (elapsed >= dur) { clearAll(); nextT = now + T(5000); subState = 3; }
  } else if (subState == 3 && now >= nextT) {
    state = (state+1) % 9;
    subState = 0;
  }
}

// ---------- Программа 12: стробоскоп (вся рама) ----------
void prog12_strobe() {
  unsigned long now = millis();
  if (subState == 0) { fill_solid(leds0,LEN_BUS0,currentColor); fill_solid(leds1,LEN_BUS1,currentColor); fill_solid(leds2,LEN_BUS2,currentColor); fill_solid(leds3,LEN_BUS3,currentColor); nextT = now + T(100); subState = 1; }
  else if (subState == 1 && now >= nextT) { clearAll(); nextT = now + T(100); subState = 0; }
}

// ---------- Программа 13: стробоскоп периметр ----------
void prog13_strobe_peri() {
  unsigned long now = millis();
  if (subState == 0) { clearAll(); edgesSet(perimeterEdges, 12, currentColor); nextT = now + T(100); subState = 1; }
  else if (subState == 1 && now >= nextT) { clearAll(); nextT = now + T(100); subState = 0; }
}

void runCurrentProgram() {
  switch (currentProg) {
    case P1_LINE: prog1_line(); break;
    case P2_PAIRS: prog2_pairs(); break;
    case P3_RECTS: prog3_rects(); break;
    case P4_BROKEN: prog4_broken(); break;
    case P5_MARCH: prog5_march(); break;
    case P6_LINES: prog6_lines(); break;
    case P7_DOTS: prog7_dots(); break;
    case P8_DOTSHAPE: prog8_dotshape(); break;
    case P9_DOTS_CONT: prog9_dots_cont(); break;
    case P10_MOVE_DOT: prog10_move_dot(); break;
    case P11_SLOWPAIRS: prog11_slowpairs(); break;
    case P12_STROBE: prog12_strobe(); break;
    case P13_STROBE_PERI: prog13_strobe_peri(); break;
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
button{font-size:16px;padding:14px 8px;margin:5px;border-radius:10px;border:none;background:#333;color:#fff;width:90%;max-width:320px}
input[type=range]{width:90%;max-width:320px}
h3{margin-top:28px}
#wheel{border-radius:50%;touch-action:none;margin-top:10px}
</style></head><body>
<h2>Управление светом</h2>

<h3>Яркость</h3>
<input type="range" min="0" max="255" value="255" oninput="fetch('/set?bri='+this.value)">

<h3>Скорость</h3>
<input type="range" min="0" max="100" value="50" oninput="fetch('/set?spd='+this.value)">

<h3>Цвет</h3>
<canvas id="wheel" width="220" height="220"></canvas>

<h3>Программы</h3>
<button onclick="prog(0)">Выключить</button>
<button onclick="prog(1)">1. Бегущая линия</button>
<button onclick="prog(2)">2. Удаление пар</button>
<button onclick="prog(3)">3. Вспышки фигур</button>
<button onclick="prog(4)">4. Ломаные фигуры</button>
<button onclick="prog(5)">5. Марш фигур</button>
<button onclick="prog(6)">6. Случайные линии</button>
<button onclick="prog(7)">7. Случайные диоды</button>
<button onclick="prog(8)">8. Диодные фигуры</button>
<button onclick="prog(9)">9. Диоды постоянные</button>
<button onclick="prog(10)">10. Движение диода</button>
<button onclick="prog(11)">11. Медленные линии</button>
<button onclick="prog(12)">12. Стробоскоп</button>
<button onclick="prog(13)">13. Строб периметр</button>

<script>
function prog(n){ fetch('/prog?n='+n); }

// Цветовое колесо
const cv = document.getElementById('wheel');
const ctx = cv.getContext('2d');
const R = 110, cx=110, cy=110;
const img = ctx.createImageData(220,220);
for (let y=0;y<220;y++){
  for (let x=0;x<220;x++){
    const dx=x-cx, dy=y-cy;
    const dist=Math.sqrt(dx*dx+dy*dy);
    const idx=(y*220+x)*4;
    if (dist<=R){
      let hue = Math.atan2(dy,dx)*180/Math.PI; if(hue<0) hue+=360;
      let sat = Math.min(1, dist/R);
      const [r,g,b] = hsv2rgb(hue,sat,1);
      img.data[idx]=r; img.data[idx+1]=g; img.data[idx+2]=b; img.data[idx+3]=255;
    } else { img.data[idx+3]=0; }
  }
}
ctx.putImageData(img,0,0);

function hsv2rgb(h,s,v){
  let c=v*s, x=c*(1-Math.abs((h/60)%2-1)), m=v-c;
  let r=0,g=0,b=0;
  if(h<60){r=c;g=x;b=0;} else if(h<120){r=x;g=c;b=0;} else if(h<180){r=0;g=c;b=x;}
  else if(h<240){r=0;g=x;b=c;} else if(h<300){r=x;g=0;b=c;} else {r=c;g=0;b=x;}
  return [Math.round((r+m)*255), Math.round((g+m)*255), Math.round((b+m)*255)];
}

function pickColor(e){
  const rect = cv.getBoundingClientRect();
  const clientX = e.touches ? e.touches[0].clientX : e.clientX;
  const clientY = e.touches ? e.touches[0].clientY : e.clientY;
  const x = clientX - rect.left, y = clientY - rect.top;
  const dx=x-cx, dy=y-cy;
  const dist=Math.sqrt(dx*dx+dy*dy);
  if (dist>R) return;
  let hue = Math.atan2(dy,dx)*180/Math.PI; if(hue<0) hue+=360;
  let sat = Math.min(1, dist/R);
  const [r,g,b] = hsv2rgb(hue,sat,1);
  const hex = ((1<<24)+(r<<16)+(g<<8)+b).toString(16).slice(1);
  fetch('/set?color='+hex);
}
cv.addEventListener('click', pickColor);
cv.addEventListener('touchstart', pickColor);
</script>
</body></html>
)HTML";

void handleRoot() { server.send(200, "text/html", PAGE); }

void handleProg() {
  int n = server.arg("n").toInt();
  currentProg = (Prog)n;
  resetProgState();
  dotPoolInit = false;
  server.send(200, "text/plain", "ok");
}

void handleSet() {
  if (server.hasArg("bri")) {
    masterBrightness = server.arg("bri").toInt();
    FastLED.setBrightness(masterBrightness);
  }
  if (server.hasArg("spd")) {
    speedFactor = mapSpeed(server.arg("spd").toInt());
  }
  if (server.hasArg("color")) {
    String hex = server.arg("color");
    long val = strtol(hex.c_str(), NULL, 16);
    currentColor = CRGB((val>>16)&0xFF, (val>>8)&0xFF, val&0xFF);
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

  buildGraph();

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
