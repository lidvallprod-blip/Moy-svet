/*
  QuinLED Dig-Quad — 18 кнопок: 2 комбо + 16 световых программ, веб-управление (цвет/яркость/скорость)
  ===========================================================================================
  Порядок кнопок на странице (сверху вниз — от спокойных к интенсивным):
   1. Комбо: бегущая линия + удаление пар (ф1 линии, ф2 линии, ф1 пар, ф3 линии, ф4 линии, ф2 пар)
   2. Комбо: вспышки фигур + марш (случайно: двойная резкая/быстрое затухание или резкая/медленное)
   3. Движение диодов (два диода + вспышка периметра раз в 6 с)
   4. Случайные диоды
   5. Медленные линии (отрезок в пол ребра едет по линии за 0.8 с)
   6. Диодные фигуры (точки медленно расползаются во время вспышки)
   7. Диодные фигуры x1 (по одному диоду на ребро)
   8. Периметр с затуханием
   9. Отрезки в пол ребра
  10. Марш фигур
  11. Бегущая линия
  12. Удаление пар (ф1 — пары, ф2 — по одному ребру, чередуются)
  13. Случайные линии
  14. Вспышки фигур
  15. Ломаные фигуры
  16. Ломаные (вся рама)
  17. Строб периметр
  18. Стробоскоп

  Мастер-регуляторы на странице (действуют на ВСЁ сразу):
   - Яркость: 0 (выкл) .. 255 (максимум)
   - Скорость: ползунок 0-100. В середине (50) = тайминги "по умолчанию".
     Влево до 0 — почти полная остановка (~x0.05). Вправо до 100 — x2.5.
   - Цвет: круглое колесо + 6 кнопок быстрого выбора. По умолчанию — красный.

  ПОСЛЕ ЗАПУСКА ПРОВЕРЬТЕ (разумные предположения там, где ТЗ допускало варианты):
   - Программа 5 (Медленные линии): направление отрезка по каждому ребру
     определено автоматически по цепочке подключения (таблица EDGE_PIX_BA).
     Для рёбер 8 и 15 (концы перемычек) значение по умолчанию. Если на
     каком-то ряду отрезок едет не в ту сторону — скажите ряд.
   - Программа 3 (Движение диодов): диодов два; периметр вспыхивает раз в 6 с.
   - Программа 6 (Диодные фигуры): каждая точка уползает на 10-15 пикселей
     в случайную сторону вдоль своего ребра.
   - Программа 13 (Случайные линии): "сдвиг на 10 диодов" применён к
     одиночному частичному ребру.
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
  return 1.0f + (2.5f - 1.0f) * ((v - 50) / 50.0f);
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
const uint8_t brokenShapes[14][16] = {
  {9,12,15,17,16}, {18,16,14}, {1,4,6}, {2,5,7}, {3,6,9,11},
  {13,16,19}, {2,5,10}, {11,14,17,20}, {8,11,12}, {7,10,12}, {16,19,21},
  {1,3,5,7,9,11,13,15,17,19,21},
  {2,4,6,8,10,12,14,16,18,20,22},
  {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}
};
const uint8_t brokenLen[14] = {5,3,3,3,4,3,3,4,3,3,3,11,11,16};

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
enum Prog { P_OFF, P_COMBO12, P_COMBO35, P10_MOVE_DOT, P7_DOTS, P11_SLOWPAIRS,
            P8_DOTSHAPE, P15_DOTSHAPE_SINGLE, P_PERI_FADE, P16_HALF_SEG, P5_MARCH,
            P1_LINE, P2_PAIRS, P6_LINES, P3_RECTS, P4_BROKEN, P14_BROKEN_ALL,
            P13_STROBE_PERI, P12_STROBE };
#define PROG_COUNT 18   // кнопки 1..18, 0 = выключить
Prog currentProg = P_OFF;
unsigned long nextT = 0;
int state = 0, subState = 0, repeatCnt = 0;
unsigned long phaseStartMs = 0;

int p2idx = 0; // номер текущей вспышки внутри фазы пресета "Удаление пар"
void resetProgState() { state = 0; subState = 0; repeatCnt = 0; nextT = 0; p2idx = 0; phaseStartMs = millis(); }

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

// ph: 0..3 = фазы 1..4. Возвращает true, когда фаза отработала 3 раза (с паузами по 5 с).
bool prog1_phase(int ph) {
  unsigned long now = millis();
  static PathStep path1[12]; static PathStep pathC[3][4];
  bool longMode = (ph==0 || ph==2);

  if (subState == 0) {
    bool up = (ph < 2);
    if (longMode) {
      PathStep tmp[4];
      getColumnPath(0, up, tmp); for(int i=0;i<4;i++) path1[i]=tmp[i];
      getColumnPath(1, up, tmp); for(int i=0;i<4;i++) path1[4+i]=tmp[i];
      getColumnPath(2, up, tmp); for(int i=0;i<4;i++) path1[8+i]=tmp[i];
    } else {
      getColumnPath(0, up, pathC[0]);
      getColumnPath(1, up, pathC[1]);
      getColumnPath(2, up, pathC[2]);
    }
    phaseStartMs = now;
    subState = 1;
  }

  unsigned long dur = longMode ? T(1667) : T(333); // скорость x3 от исходной

  if (subState == 1) {
    unsigned long elapsed = now - phaseStartMs; if (elapsed>dur) elapsed=dur;
    float frac = (float)elapsed/dur;
    clearAll();
    if (longMode) {
      drawMovingWindow(path1, 12, frac);
    } else {
      for (int c=0;c<3;c++) drawMovingWindow(pathC[c], 4, frac);
    }
    if (now - phaseStartMs >= dur) { subState = 2; nextT = now + T(5000); clearAll(); }
  } else if (subState == 2) {
    if (now >= nextT) {
      repeatCnt++;
      subState = 0;
      if (repeatCnt >= 3) { repeatCnt = 0; return true; }
    }
  }
  return false;
}

void prog1_line() { if (prog1_phase(state)) state = (state+1) % 4; }

// ---------- Программа 2: удаление пар (резко) ----------
const uint8_t pairSeq[5][2] = {{21,22},{16,17},{11,12},{6,7},{1,2}};
const uint8_t singleSeq[10] = {21,22,16,17,11,12,6,7,1,2};
// ph 0: пары рёбер по 0.2 с; ph 1: по одному ребру по 0.1 с (вся фаза ~1 с).
// Возвращает true, когда фаза закончилась (включая паузу 6 с).
bool prog2_phase(int ph) {
  unsigned long now = millis();
  int total = (ph == 0) ? 5 : 10;
  unsigned long flashMs = (ph == 0) ? T(200) : T(100);
  if (subState == 0) {
    clearAll();
    if (ph == 0) edgesSet(pairSeq[p2idx], 2, currentColor); else edgeSet(singleSeq[p2idx], currentColor);
    nextT = now + flashMs; subState = 1;
  } else if (subState == 1 && now >= nextT) {
    clearAll();
    p2idx++;
    if (p2idx >= total) { p2idx = 0; nextT = now + T(6000); subState = 2; }
    else subState = 0;
  } else if (subState == 2 && now >= nextT) { subState = 0; return true; }
  return false;
}

void prog2_pairs() { if (prog2_phase(state)) state = 1 - state; }

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
  else if (subState == 3 && now >= nextT) { clearAll(); nextT = now + T(50); subState = 4; }
  else if (subState == 4 && now >= nextT) { edgesSet(buf, len, currentColor); nextT = now + T(50); subState = 5; }
  else if (subState == 5 && now >= nextT) { clearAll(); nextT = now + T(6000); subState = 6; }
  else if (subState == 6 && now >= nextT) { subState = 0; }
}

// ---------- Программа 4: ломаные фигуры (двойная вспышка, с пробелами) ----------
void prog4_broken() {
  unsigned long now = millis();
  static int idx;
  if (subState == 0) {
    idx = random(0,14);
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
    unsigned long dur = T(1600); // затухание x1.6 от исходной 1000мс
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

// ---------- Программа 6: случайные вспышки линий (шестерная вспышка) ----------
void prog6_lines() {
  unsigned long now = millis();
  static uint8_t buf[3]; static uint8_t len; static bool partial; static float pStart;
  static int flashCount;
  if (subState == 0) {
    int mode = random(0,3); // 0=одиночное частичное ребро, 1=цепочка колонны, 2=пара ряда
    if (mode == 0) {
      const uint8_t* col = (random(0,3)==0)?colL:(random(0,2)==0?colM:colR);
      buf[0] = col[random(0,4)]; len = 1; partial = true;
      int ei = buf[0]-1;
      int elen = edgeStop[ei]-edgeStart[ei];
      pStart = random(0,50)/100.0 - (10.0/elen); // сдвиг на 10 диодов "вверх" по калибровке
      if (pStart < 0) pStart = 0;
    } else if (mode == 2) {
      int r = random(0,5); buf[0]=rowH[r][0]; buf[1]=rowH[r][1]; len=2; partial=false;
    } else {
      const uint8_t* col = (random(0,3)==0)?colL:(random(0,2)==0?colM:colR);
      int chainLen = random(2,4); // 2 или 3
      int s = random(0,4-chainLen+1);
      for (int k=0;k<chainLen;k++) buf[k]=col[s+k];
      len = chainLen; partial=false;
    }
    flashCount = 0;
    subState = 1;
  }
  else if (subState == 1) { // зажигаем очередную вспышку
    if(partial) drawPartialEdge(buf[0],pStart,0.4,currentColor); else edgesSet(buf,len,currentColor);
    nextT = now + T(80); subState = 2;
  }
  else if (subState == 2 && now >= nextT) { // гасим, считаем вспышки
    clearAll();
    flashCount++;
    if (flashCount >= 6) { subState = 5; phaseStartMs = now; }
    else { nextT = now + T(80); subState = 3; }
  }
  else if (subState == 3 && now >= nextT) { subState = 1; }
  else if (subState == 5) {
    unsigned long dur = T(1120); // затухание x1.6 от исходных 700мс
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

// ---------- Программа 8: диодные фигуры (2 точки на ребро), точки медленно расползаются ----------
int8_t d8dir[23][2]; uint8_t d8dist[23][2];
void drawDots8(uint8_t scale, float prog) {
  clearAll();
  CRGB c = scaledColor(scale);
  for (int e=1;e<=22;e++){
    int s = edgeStart[e-1], L = edgeStop[e-1]-s;
    int base[2] = { L/3, (2*L)/3 };
    for (int k=0;k<2;k++) {
      int p = base[k] + (int)(d8dir[e][k] * d8dist[e][k] * prog);
      if (p < 0) p = 0;
      if (p > L-1) p = L-1;
      setGlobal(s + p, c);
    }
  }
}
void prog8_dotshape() {
  unsigned long now = millis();
  unsigned long rise = T(800), fall = T(1280);
  if (subState == 0) {
    for (int e=1;e<=22;e++) for (int k=0;k<2;k++) { d8dir[e][k] = random(0,2) ? 1 : -1; d8dist[e][k] = random(10,16); }
    phaseStartMs = now; subState = 1;
  }
  if (subState == 1) {
    unsigned long elapsed = now - phaseStartMs; if (elapsed>rise) elapsed=rise;
    drawDots8((uint8_t)(255.0*elapsed/rise), (float)elapsed/(rise+fall));
    if (elapsed >= rise) { subState = 2; phaseStartMs = now; }
  } else if (subState == 2) {
    unsigned long elapsed = now - phaseStartMs; if (elapsed>fall) elapsed=fall;
    drawDots8(255 - (uint8_t)(255.0*elapsed/fall), (float)(rise+elapsed)/(rise+fall));
    if (elapsed >= fall) { clearAll(); nextT = now + T(6000); subState = 3; }
  } else if (subState == 3 && now >= nextT) { subState = 0; }
}

// ---------- Программа 10: два диода + вспышка периметра раз в 6 с ----------
void prog10_move_dot() {
  unsigned long now = millis();
  static int curEdge[2] = {-1,-1}, fromNode[2], toNode[2];
  static uint8_t curBrightness[2];
  static unsigned long dotStartMs[2];
  static unsigned long perimStart = 0;

  for (int d=0; d<2; d++) {
    if (curEdge[d] == -1) {
      curEdge[d] = (d==0) ? 1 : 12; // стартуют с разных рёбер
      fromNode[d] = edgeNodeA[curEdge[d]]; toNode[d] = edgeNodeB[curEdge[d]];
      dotStartMs[d] = now; curBrightness[d] = random(140,255);
    }
  }

  clearAll();

  // Вспышка периметра: 0.8 с нарастание + 0.8 с спад (как в "Медленных линиях"), цикл 6 с
  unsigned long cyc = T(6000), rise = T(800);
  if (perimStart == 0 || now - perimStart >= cyc) perimStart = now;
  unsigned long pe = now - perimStart;
  float pk = 0;
  if (pe < rise) pk = (float)pe / rise;
  else if (pe < 2*rise) pk = 1.0 - (float)(pe - rise) / rise;
  if (pk > 0) edgesSet(perimeterEdges, 12, scaledColor((uint8_t)(255*pk)));

  for (int d=0; d<2; d++) {
    unsigned long dur = T(2000);
    unsigned long elapsed = now - dotStartMs[d]; if (elapsed>dur) elapsed=dur;
    float frac = (float)elapsed/dur;

    int i = curEdge[d] - 1;
    int len = edgeStop[i]-edgeStart[i];
    bool goForward = (fromNode[d] == edgeNodeA[curEdge[d]]);
    int pos = goForward ? (int)(frac*len) : (int)((1-frac)*len);
    setGlobal(edgeStart[i]+pos, scaledColor(curBrightness[d]));

    if (elapsed >= dur) {
      int node = toNode[d];
      int deg = adjDegree[node];
      if (deg > 0) {
        int pick;
        int tries = 0;
        do { pick = adjEdges[node][random(0,deg)]; tries++; } while (pick == curEdge[d] && deg>1 && tries<5);
        int newFrom = node;
        int newTo = (edgeNodeA[pick]==node) ? edgeNodeB[pick] : edgeNodeA[pick];
        curEdge[d] = pick; fromNode[d] = newFrom; toNode[d] = newTo;
      }
      dotStartMs[d] = now; curBrightness[d] = random(140,255);
    }
  }
}

// ---------- Программа 11: медленные линии — отрезок в пол ребра едет по линии за 0.8 с ----------
const int slowSeq[9] = {0,1,2,3,4,3,2,1,0}; // индексы в pairSeq, вперёд и назад
// true = пиксели ленты на этом ребре идут "справа налево" / "снизу вверх" (B->A).
// Определено по цепочке подключения рёбер; для рёбер 8 и 15 (концы перемычек) — по умолчанию false.
// Индекс = номер ребра (1..22), нулевой элемент не используется.
const bool EDGE_PIX_BA[23] = {
  false,
  false, false, true,  true,  false,   // 1-5
  true,  true,  false, true,  true,    // 6-10
  true,  false, false, false, false,   // 11-15
  true,  false, false, true,  false,   // 16-20
  false, true                          // 21-22
};
void prog11_slowpairs() {
  unsigned long now = millis();
  static PathStep path[2];
  if (subState == 0) {
    int pi = slowSeq[state];
    bool ltr = (state <= 4); // вперёд (снизу вверх по раме) — слева направо, обратно — справа налево
    int eL = pairSeq[pi][0], eR = pairSeq[pi][1];
    if (ltr) {
      path[0].edge = eL; path[0].rev = EDGE_PIX_BA[eL];
      path[1].edge = eR; path[1].rev = EDGE_PIX_BA[eR];
    } else {
      path[0].edge = eR; path[0].rev = !EDGE_PIX_BA[eR];
      path[1].edge = eL; path[1].rev = !EDGE_PIX_BA[eL];
    }
    phaseStartMs = now; subState = 1;
  }
  if (subState == 1) {
    unsigned long dur = T(800);
    unsigned long elapsed = now - phaseStartMs; if (elapsed>dur) elapsed=dur;
    clearAll();
    drawMovingWindow(path, 2, (float)elapsed/dur);
    if (elapsed >= dur) { clearAll(); nextT = now + T(5000); subState = 2; }
  } else if (subState == 2 && now >= nextT) {
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

// ---------- Программа 14: ломаные фигуры по всей конструкции ----------
void prog14_broken_all() {
  unsigned long now = millis();
  if (subState == 0) {
    clearAll();
    for (int e=1;e<=22;e++) drawJaggedEdge(e, currentColor);
    nextT = now + T(50); subState = 1;
  } else if (subState == 1 && now >= nextT) { clearAll(); nextT = now + T(50); subState = 2; }
  else if (subState == 2 && now >= nextT) {
    for (int e=1;e<=22;e++) drawJaggedEdge(e, currentColor);
    nextT = now + T(50); subState = 3;
  } else if (subState == 3 && now >= nextT) { clearAll(); nextT = now + T(6000); subState = 4; }
  else if (subState == 4 && now >= nextT) { subState = 0; }
}

// ---------- Программа 15: диодные фигуры, один диод на ребро ----------
void prog15_dotshape_single() {
  unsigned long now = millis();
  if (subState == 0) { phaseStartMs = now; subState = 1; }
  if (subState == 1) {
    unsigned long dur = T(800);
    unsigned long elapsed = now - phaseStartMs; if (elapsed>dur) elapsed=dur;
    uint8_t scale = (uint8_t)(255.0*elapsed/dur);
    clearAll();
    for (int e=1;e<=22;e++){
      int L = edgeStop[e-1]-edgeStart[e-1];
      setGlobal(edgeStart[e-1] + L/2, scaledColor(scale));
    }
    if (elapsed >= dur) { subState = 2; phaseStartMs = now; }
  } else if (subState == 2) {
    unsigned long dur = T(1280);
    unsigned long elapsed = now - phaseStartMs; if (elapsed>dur) elapsed=dur;
    uint8_t scale = 255 - (uint8_t)(255.0*elapsed/dur);
    clearAll();
    for (int e=1;e<=22;e++){
      int L = edgeStop[e-1]-edgeStart[e-1];
      setGlobal(edgeStart[e-1] + L/2, scaledColor(scale));
    }
    if (elapsed >= dur) { clearAll(); nextT = now + T(6000); subState = 3; }
  } else if (subState == 3 && now >= nextT) { subState = 0; }
}

// ---------- Программа 16: вспышки отрезков в пол ребра, резко/угасание 1.5с, без пауз ----------
void prog16_half_segments() {
  unsigned long now = millis();
  static int segEdge; static float segStart;
  if (subState == 0) {
    segEdge = random(1,23);
    segStart = random(0,50)/100.0; // чтобы пол-ребра уместилось целиком
    clearAll();
    drawPartialEdge(segEdge, segStart, 0.5, currentColor);
    phaseStartMs = now; subState = 1;
  } else if (subState == 1) {
    unsigned long dur = T(1500);
    unsigned long elapsed = now - phaseStartMs; if (elapsed>dur) elapsed=dur;
    uint8_t scale = 255 - (uint8_t)(255.0*elapsed/dur);
    clearAll();
    drawPartialEdge(segEdge, segStart, 0.5, scaledColor(scale));
    if (elapsed >= dur) { subState = 0; } // сразу следующий отрезок, без паузы
  }
}

// ---------- Периметр с плавным затуханием (как фаза 1 пресета "Марш фигур") ----------
void prog_peri_fade() {
  unsigned long now = millis();
  if (subState == 0) { clearAll(); edgesSet(perimeterEdges, 12, currentColor); nextT = now + T(200); subState = 1; }
  else if (subState == 1 && now >= nextT) { phaseStartMs = now; subState = 2; }
  else if (subState == 2) {
    unsigned long dur = T(1600);
    unsigned long elapsed = now - phaseStartMs; if (elapsed>dur) elapsed=dur;
    uint8_t scale = 255 - (uint8_t)(255.0 * elapsed / dur);
    clearAll(); edgesSet(perimeterEdges, 12, scaledColor(scale));
    if (elapsed >= dur) { clearAll(); nextT = now + T(6000); subState = 3; }
  } else if (subState == 3 && now >= nextT) { subState = 0; }
}

// ---------- Комбо 1+2: бегущая линия + удаление пар ----------
// Порядок: линия ф1, линия ф2, пары ф1, линия ф3, линия ф4, пары ф2 — и по кругу.
// Фазы бегущей линии идут по 3 повтора, как в обычном пресете.
void prog_combo12() {
  bool done;
  switch (state) {
    case 0: done = prog1_phase(0); break;
    case 1: done = prog1_phase(1); break;
    case 2: done = prog2_phase(0); break;
    case 3: done = prog1_phase(2); break;
    case 4: done = prog1_phase(3); break;
    default: done = prog2_phase(1); break;
  }
  if (done) state = (state + 1) % 6;
}

// ---------- Комбо 3+5: случайно чередуются два типа вспышек ----------
// Тип А (как пресет 3): случайная фигура, двойная резкая вспышка + быстрое затухание 0.3 с.
// Тип Б (как пресет 5): случайный ряд/квадрат, одна резкая вспышка + медленное затухание 1.6 с.
// Между вспышками 6 с.
void prog_combo35() {
  unsigned long now = millis();
  static uint8_t buf[12]; static uint8_t len; static bool slowType;
  if (subState == 0) {
    slowType = (random(0,2) == 1);
    if (slowType) {
      int variant = random(0,3); // 0 = во всю ширину, 1 = квадрат справа, 2 = квадрат слева
      int row = random(0,4);
      buildRect(variant==0 ? 2 : (variant==1 ? 1 : 0), row, row, buf, len);
    } else {
      int colMode = random(0,3);
      int rowStart = random(0,4);
      int rowEnd = rowStart + random(0, 4-rowStart);
      buildRect(colMode, rowStart, rowEnd, buf, len);
    }
    clearAll(); edgesSet(buf, len, currentColor);
    nextT = now + (slowType ? T(200) : T(50)); subState = 1;
  } else if (subState == 1 && now >= nextT) {
    if (slowType) { phaseStartMs = now; subState = 5; }
    else { clearAll(); nextT = now + T(50); subState = 2; }
  } else if (subState == 2 && now >= nextT) { edgesSet(buf, len, currentColor); nextT = now + T(50); subState = 3; }
  else if (subState == 3 && now >= nextT) { phaseStartMs = now; subState = 4; }
  else if (subState == 4 || subState == 5) {
    unsigned long dur = (subState == 5) ? T(1600) : T(300);
    unsigned long elapsed = now - phaseStartMs; if (elapsed>dur) elapsed=dur;
    uint8_t scale = 255 - (uint8_t)(255.0 * elapsed / dur);
    clearAll(); edgesSet(buf, len, scaledColor(scale));
    if (elapsed >= dur) { clearAll(); nextT = now + T(6000); subState = 6; }
  } else if (subState == 6 && now >= nextT) { subState = 0; }
}

void runCurrentProgram() {
  switch (currentProg) {
    case P_COMBO12: prog_combo12(); break;
    case P_COMBO35: prog_combo35(); break;
    case P10_MOVE_DOT: prog10_move_dot(); break;
    case P7_DOTS: prog7_dots(); break;
    case P11_SLOWPAIRS: prog11_slowpairs(); break;
    case P8_DOTSHAPE: prog8_dotshape(); break;
    case P15_DOTSHAPE_SINGLE: prog15_dotshape_single(); break;
    case P_PERI_FADE: prog_peri_fade(); break;
    case P16_HALF_SEG: prog16_half_segments(); break;
    case P5_MARCH: prog5_march(); break;
    case P1_LINE: prog1_line(); break;
    case P2_PAIRS: prog2_pairs(); break;
    case P6_LINES: prog6_lines(); break;
    case P3_RECTS: prog3_rects(); break;
    case P4_BROKEN: prog4_broken(); break;
    case P14_BROKEN_ALL: prog14_broken_all(); break;
    case P13_STROBE_PERI: prog13_strobe_peri(); break;
    case P12_STROBE: prog12_strobe(); break;
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
.colorbtn{width:auto;display:inline-block;padding:10px 14px;margin:4px}
</style></head><body>
<h2>Управление светом</h2>

<h3>Яркость</h3>
<input type="range" min="0" max="255" value="255" oninput="fetch('/set?bri='+this.value)">

<h3>Скорость</h3>
<input type="range" min="0" max="100" value="50" oninput="fetch('/set?spd='+this.value)">

<h3>Цвет</h3>
<canvas id="wheel" width="220" height="220"></canvas>
<div>
<button class="colorbtn" onclick="setColor('FF0000')">Красный</button>
<button class="colorbtn" onclick="setColor('FFFFFF')">Белый</button>
<button class="colorbtn" onclick="setColor('0040FF')">Синий</button>
<button class="colorbtn" onclick="setColor('00FF00')">Зелёный</button>
<button class="colorbtn" onclick="setColor('CBA135')">Горчичный</button>
<button class="colorbtn" onclick="setColor('FF1493')">Розовый</button>
</div>

<h3>Программы</h3>
<button onclick="prog(0)">Выключить</button>
<button onclick="prog(1)">1. Комбо: бегущая линия + удаление пар</button>
<button onclick="prog(2)">2. Комбо: вспышки фигур + марш</button>
<button onclick="prog(3)">3. Движение диодов</button>
<button onclick="prog(4)">4. Случайные диоды</button>
<button onclick="prog(5)">5. Медленные линии</button>
<button onclick="prog(6)">6. Диодные фигуры</button>
<button onclick="prog(7)">7. Диодные фигуры x1</button>
<button onclick="prog(8)">8. Периметр с затуханием</button>
<button onclick="prog(9)">9. Отрезки в пол ребра</button>
<button onclick="prog(10)">10. Марш фигур</button>
<button onclick="prog(11)">11. Бегущая линия</button>
<button onclick="prog(12)">12. Удаление пар</button>
<button onclick="prog(13)">13. Случайные линии</button>
<button onclick="prog(14)">14. Вспышки фигур</button>
<button onclick="prog(15)">15. Ломаные фигуры</button>
<button onclick="prog(16)">16. Ломаные (вся рама)</button>
<button onclick="prog(17)">17. Строб периметр</button>
<button onclick="prog(18)">18. Стробоскоп</button>

<script>
function prog(n){ fetch('/prog?n='+n); }
function setColor(hex){ fetch('/set?color='+hex); }

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
  if (n < 0 || n > PROG_COUNT) n = 0;
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
