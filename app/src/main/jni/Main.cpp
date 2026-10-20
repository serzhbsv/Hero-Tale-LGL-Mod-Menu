#include <list>
#include <vector>
#include <cstring>
#include <pthread.h>
#include <string>
#include <jni.h>
#include <unistd.h>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.hpp"
#include "Menu/Menu.hpp"
#include "Menu/Jni.hpp"
#include "Includes/Macros.h"

// ============================================================
//  Глобальные переменные
// ============================================================

// --- Основное ---
bool godMode          = false;   // Режим бога
bool coinMultEnabled  = false;   // Бонус монет
bool forceDropEnabled = false;   // Гарантия дропа

// --- Опыт ---
bool expLevelEnabled    = false;
bool expCombatEnabled   = false;
bool expDistanceEnabled = false;
bool expMagicEnabled    = false;

double multLevel    = 1.0;
double multCombat   = 1.0;
double multDistance = 1.0;
double multMagic    = 1.0;

int coinMult = 1;

// --- Бонус Distance (лук/рогатка) ---
bool  distanceBonusEnabled = false;
float distanceBonus        = 210.0f;
void* g_playerStatFile     = nullptr;
float g_originalDistance   = 0.0f;
bool  g_originalSaved      = false;

// --- Бонус Speed (скорость атаки, все оружия) ---
bool  speedBonusEnabled = false;
float speedBonus        = 100.0f;
float g_originalSpeed   = 0.0f;
bool  g_speedSaved      = false;

// --- Бонус Attack (урон, все оружия) ---
bool  attackBonusEnabled = false;
float attackBonus        = 1000.0f;
float g_originalAttack   = 0.0f;
bool  g_attackSaved      = false;

// --- Позиция лута ---
float lastLootPos[3] = {0, 0, 0};
bool  hasLootPos     = false;

// --- Кэш множителя монет ---
std::unordered_map<void*, uint32_t> coinLastMult;

// --- Защита от повторного форсирования дропа ---
std::unordered_set<void*> g_forcedItems;
std::mutex g_forcedItemsMutex;

struct Vector3 {
    float x, y, z;
};

// Аналог GetTickCount() для Android
static inline uint32_t GetTickCountMs() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

// ============================================================
//  GetObjectName — читает Il2CppString* name (offset 0x18)
// ============================================================
char* GetObjectName(void* __this) {
    static char buf[256];
    buf[0] = 0;
    if (__this == nullptr) return buf;

#if defined(__aarch64__)
    void* strPtr = *(void**)((unsigned char*)__this + 0x18);
    int lengthOffset = 0x10;
    int charsOffset  = 0x14;
#elif defined(__arm__)
    void* strPtr = *(void**)((unsigned char*)__this + 0xC);
    int lengthOffset = 0x8;
    int charsOffset  = 0xC;
#endif

    if (strPtr == nullptr) return buf;
    unsigned char* str = (unsigned char*)strPtr;
    int len = *(int*)(str + lengthOffset);
    if (len <= 0 || len >= 256) return buf;
    for (int i = 0; i < len; i++)
        buf[i] = str[charsOffset + i * 2];
    buf[len] = 0;
    return buf;
}

// ============================================================
//  Typedef'ы оригинальных функций
// ============================================================

// Stat.AddExp(long exp)
void (*old_AddExp)(void*, int64_t);

// Hero.GetDmg(int dmg, int damageType, int* dealtDamage) -> bool
bool (*old_GetDmg)(void*, int, int, int*);

// CombatObject.ApplyHp(int h, bool showPopText)
void (*old_ApplyHp)(void*, int, bool);

// Coin.InitCoin(void* ground, void* pouch)
void (*old_InitCoin)(void*, void*, void*);

// Coin.ShowCoin(float angle, void* dir, void* pos, bool isBonus, void* glowColor)





#if defined(__aarch64__)
    // ARM64 — старый (работал)
    typedef void (*ShowCoin_t)(void*, float, void*, void*, bool, void*);
#elif defined(__arm__)
    // ARMv7 — правильный
    typedef void (*ShowCoin_t)(void*, float, Vector3, Vector3, bool, void*);
#endif
ShowCoin_t old_ShowCoin = nullptr;



// LootManager.CreateLoot(Enemy enemy)
void (*old_CreateLoot)(void*, void*);

// LootManager.ItemsToDrop(List<ItemData>, List<int>, Vector3*, float)
void (*old_ItemsToDrop)(void*, void*, void*, void*, float);

// LootManager.ItemHasReachBag(ItemData itemData)
void (*old_ItemHasReachBag)(void*, void*);

// Enemy.DrawItems() -> List<ItemData>
void* (*old_DrawItems)(void*);

// LootItem.Activation(ItemData, int rarity, float delay, float xDist, Vector3*, float endH)
#if defined(__aarch64__)
    typedef void (*LootItemActivation_t)(void*, void*, int, float, float, void*, float);
#elif defined(__arm__)
    typedef void (*LootItemActivation_t)(void*, void*, int, float, float, Vector3, float);
#endif
LootItemActivation_t old_LootItemActivation = nullptr;

// HeroStats.AttackDone(bool isRanged)
void (*old_AttackDone)(void*, bool);

// ============================================================
//  Хуки
// ============================================================

// ------------------------------------------------------------
//  Класс: Stat
//  Функция: AddExp(long exp)
//  RVA (Android): 0x186DD88
//  Что делает: начисление опыта (Level/Combat/Distance/Magic)
//  Чит: бонус к опыту (+multX) по каждой категории отдельно
//  Примечание: имя категории читается из Stat.name (offset 0x18)
// ------------------------------------------------------------
void Hook_AddExp(void* __this, int64_t exp) {
    if (__this == nullptr) { old_AddExp(__this, exp); return; }
    char* name = GetObjectName(__this);

    double newExp = (double)exp;

    if (strcmp(name, "Level") == 0 && expLevelEnabled)
        newExp = exp + multLevel;
    else if (strcmp(name, "Combat") == 0 && expCombatEnabled)
        newExp = exp + multCombat;
    else if (strcmp(name, "Distance") == 0 && expDistanceEnabled)
        newExp = exp + multDistance;
    else if (strcmp(name, "Magic") == 0 && expMagicEnabled)
        newExp = exp + multMagic;

    // Защита от переполнения int32
    const double SAFE_MAX = 2000000000.0;
    if (newExp > SAFE_MAX) newExp = SAFE_MAX;
    if (newExp < 0)        newExp = (double)exp;

    exp = (int64_t)newExp;
    LOGI(OBFUSCATE("[AddExp] stat=%s exp=%lld"), name, (long long)exp);
    old_AddExp(__this, exp);
}

// ------------------------------------------------------------
//  Класс: Hero
//  Функция: GetDmg(int dmg, int damageType, int* dealtDamage) -> bool
//  RVA (Android): 0x184C5B0
//  Что делает: расчёт входящего урона
//  Чит: God Mode — блокирует весь урон, возвращает false
// ------------------------------------------------------------
bool Hook_GetDmg(void* __this, int dmg, int damageType, int* dealtDamage) {
    if (godMode) {
        if (dealtDamage) *dealtDamage = 0;
        LOGI(OBFUSCATE("[GetDmg] GOD MODE: blocked %d dmg"), dmg);
        return false;
    }
    return old_GetDmg(__this, dmg, damageType, dealtDamage);
}

// ------------------------------------------------------------
//  Класс: CombatObject
//  Функция: ApplyHp(int h, bool showPopText)
//  RVA (Android): 0x1845FC4
//  Что делает: применение HP (лечение/урон)
//  Чит: God Mode — блокирует отрицательное HP (урон)
// ------------------------------------------------------------
void Hook_ApplyHp(void* __this, int h, bool showPopText) {
    if (godMode && h < 0) return;
    old_ApplyHp(__this, h, showPopText);
}

// ------------------------------------------------------------
//  Класс: Coin
//  Функция: ShowCoin(float angle, void* dir, void* pos, bool isBonus, void* glowColor)
//  RVA (Android): 0x188A148
//  Что делает: отображение монеты + захват позиции дропа
//  Чит: прибавка монет (coinMult) + сохранение lastLootPos
//  Примечание: поле value — offset 0x20 в Coin
// ------------------------------------------------------------
#if defined(__aarch64__)
void Hook_ShowCoin(void* __this, float angle, void* dir, void* pos, bool isBonus, void* glowColor) {
    if (pos) {
        float* p = (float*)pos;
        lastLootPos[0] = p[0];
        lastLootPos[1] = p[1];
        lastLootPos[2] = p[2];
        hasLootPos = true;
    }
    int* valuePtr = (int*)((unsigned char*)__this + 0x20);
    if (coinMultEnabled) {
        uint32_t now = GetTickCountMs();
        uint32_t last = coinLastMult[__this];
        if (now - last > 200) {
            *valuePtr = *valuePtr + coinMult;
            coinLastMult[__this] = now;
        }
    } else {
        if (*valuePtr != 1) {
            *valuePtr = 1;
            coinLastMult.erase(__this);
        }
    }
    old_ShowCoin(__this, angle, dir, pos, isBonus, glowColor);
}
#elif defined(__arm__)
//  Примечание: поле value — offset 0x10 в Coin
void Hook_ShowCoin(void* __this, float angle, Vector3 dir, Vector3 pos, bool isBonus, void* glowColor) {
    lastLootPos[0] = pos.x;
    lastLootPos[1] = pos.y;
    lastLootPos[2] = pos.z;
    hasLootPos = true;

    int* valuePtr = (int*)((unsigned char*)__this + 0x10);
    if (coinMultEnabled) {
        uint32_t now = GetTickCountMs();
        uint32_t last = coinLastMult[__this];
        if (now - last > 200) {
            *valuePtr = *valuePtr + coinMult;
            coinLastMult[__this] = now;
        }
    } else {
        if (*valuePtr != 1) {
            *valuePtr = 1;
            coinLastMult.erase(__this);
        }
    }
    old_ShowCoin(__this, angle, dir, pos, isBonus, glowColor);
}
#endif

// ------------------------------------------------------------
//  Класс: LootManager
//  Функция: CreateLoot(Enemy enemy)
//  RVA (Android): 0x1851EF4
//  Что делает: создание лута в начале боя
//  Чит: сброс флагов бонусов (distance/speed/attack)
//        + очистка g_forcedItems + hasLootPos
// ------------------------------------------------------------
void Hook_CreateLoot(void* __this, void* enemy) {
    {
        std::lock_guard<std::mutex> lock(g_forcedItemsMutex);
        g_forcedItems.clear();
    }
    hasLootPos = false;
    g_originalSaved    = false;
    g_originalDistance = 0.0f;
    g_speedSaved       = false;
    g_originalSpeed    = 0.0f;
    g_attackSaved      = false;
    g_originalAttack   = 0.0f;
    old_CreateLoot(__this, enemy);
}

// ------------------------------------------------------------
//  Класс: Enemy
//  Функция: DrawItems() -> List<ItemData>
//  RVA (Android): 0x1849B30
//  Что делает: список выпадающих предметов
//  Чит: гарантированный дроп — принудительно count = 1
//  Примечание: List<ItemData> — +0x10 = items, +0x18 = count
//               элемент массива — +0x20 + i*8
//               ItemData.count — offset 0x14
// ------------------------------------------------------------
void* Hook_DrawItems(void* __this) {
    void* result = old_DrawItems(__this);
    if (result == nullptr || !forceDropEnabled) return result;

#if defined(__aarch64__)
    // ARM64
    void* itemsArray = *(void**)((unsigned char*)result + 0x10);
    int itemsCount = *(int*)((unsigned char*)result + 0x18);
    uintptr_t vectorOffset = 0x20;
    int step = 8;
    uintptr_t countOffset = 0x14;
#elif defined(__arm__)
    // ARMv7
    void* itemsArray = *(void**)((unsigned char*)result + 0x8);
    int itemsCount = *(int*)((unsigned char*)result + 0xC);
    uintptr_t vectorOffset = 0x10;
    int step = 4;
    uintptr_t countOffset = 0xC;   // ← ItemData.count
#endif

    if (itemsArray != nullptr && itemsCount > 0 && itemsCount < 1000) {
        std::lock_guard<std::mutex> lock(g_forcedItemsMutex);
        for (int i = 0; i < itemsCount; i++) {
            void* itemData = *(void**)((unsigned char*)itemsArray + vectorOffset + i * step);
            if (itemData == nullptr) continue;
            if (g_forcedItems.find(itemData) == g_forcedItems.end()) {
                int count = *(int*)((unsigned char*)itemData + countOffset);
                if (count <= 0) {
                    *(int*)((unsigned char*)itemData + countOffset) = 1;
                    LOGI(OBFUSCATE("[DrawItems] FORCED count=1 for item %d"), i);
                }
                g_forcedItems.insert(itemData);
            }
        }
    }
    return result;
}

// ------------------------------------------------------------
//  Класс: LootManager
//  Функция: ItemHasReachBag(ItemData itemData)
//  RVA (Android): 0x1852BA0
//  Что делает: предмет дошёл до сумки
//  Чит: только лог (id / count)
//  Примечание: ItemData.id — offset 0x10, count — offset 0x14
// ------------------------------------------------------------
void Hook_ItemHasReachBag(void* __this, void* itemData) {
    if (itemData) {
#if defined(__aarch64__)
        int id    = *(int*)((unsigned char*)itemData + 0x10);
        int count = *(int*)((unsigned char*)itemData + 0x14);
#elif defined(__arm__)
        int id    = *(int*)((unsigned char*)itemData + 0x8);
        int count = *(int*)((unsigned char*)itemData + 0xC);
#endif
        LOGI(OBFUSCATE("[ItemHasReachBag] id=%d count=%d"), id, count);
    }
    old_ItemHasReachBag(__this, itemData);
}
// ------------------------------------------------------------
//  Класс: LootItem
//  Функция: Activation(ItemData, int rarity, float delay, float xDist, Vector3*, float endH)
//  RVA (Android): 0x1869C0C
//  Что делает: появление коробки на экране
//  Чит: только лог (можно добавить телепорт/притяжение лута)
// ------------------------------------------------------------
#if defined(__aarch64__)
void Hook_LootItemActivation(void* __this, void* item, int rarity,
                             float delayTime, float xDistance,
                             void* startPos, float endHeight) {
    old_LootItemActivation(__this, item, rarity, delayTime, xDistance, startPos, endHeight);
}
#elif defined(__arm__)
void Hook_LootItemActivation(void* __this, void* item, int rarity,
                             float delayTime, float xDistance,
                             Vector3 startPos, float endHeight) {
    old_LootItemActivation(__this, item, rarity, delayTime, xDistance, startPos, endHeight);
}
#endif

// ------------------------------------------------------------
//  Класс: HeroStats
//  Функция: AttackDone(bool isRanged)
//  RVA (Android): 0x186DA4C
//  Что делает: завершение атаки (игрок атаковал)
//  Чит: бонусы к статам перед каждой атакой:
//        - distance (offset 0x30) — только ranged (лук/рогатка)
//        - speed    (offset 0x38) — все оружия
//        - attack   (offset 0x20) — все оружия
//  Примечание: baseStats — offset 0x90 в HeroStats
//               (StatFile) содержит статы игрока
// ------------------------------------------------------------
void Hook_AttackDone(void* __this, bool isRanged) {
    if (__this != nullptr) {

#if defined(__aarch64__)
        // ===== ARM64 =====
        const uintptr_t OFFSET_BASE_STATS = 0x90;
        const uintptr_t OFFSET_ATTACK     = 0x20;
        const uintptr_t OFFSET_DISTANCE   = 0x30;
        const uintptr_t OFFSET_SPEED      = 0x38;
#elif defined(__arm__)
        // ===== ARMv7 =====
        const uintptr_t OFFSET_BASE_STATS = 0x48;   // ← ИЗ dump.cs
        const uintptr_t OFFSET_ATTACK     = 0x18;   // ← ИЗ dump.cs
        const uintptr_t OFFSET_DISTANCE   = 0x28;   // ← ИЗ dump.cs
        const uintptr_t OFFSET_SPEED      = 0x30;   // ← ИЗ dump.cs
#else
        const uintptr_t OFFSET_BASE_STATS = 0x90;
        const uintptr_t OFFSET_ATTACK     = 0x20;
        const uintptr_t OFFSET_DISTANCE   = 0x30;
        const uintptr_t OFFSET_SPEED      = 0x38;
#endif

        void* statFile = *(void**)((unsigned char*)__this + OFFSET_BASE_STATS);
        if (statFile != nullptr) {
            g_playerStatFile = statFile;

            // --- Distance (только ranged) ---
            if (distanceBonusEnabled && isRanged) {
                float* distance = (float*)((unsigned char*)statFile + OFFSET_DISTANCE);
                if (!g_originalSaved) {
                    g_originalDistance = *distance;
                    g_originalSaved = true;
                    LOGI(OBFUSCATE("[AttackDone] saved original distance = %.2f"), g_originalDistance);
                }
                *distance = g_originalDistance + distanceBonus;
                LOGI(OBFUSCATE("[AttackDone] distance = %.2f"), *distance);
            }
            else if (!distanceBonusEnabled && g_originalSaved) {
                float* distance = (float*)((unsigned char*)statFile + OFFSET_DISTANCE);
                *distance = g_originalDistance;
                LOGI(OBFUSCATE("[AttackDone] restored distance = %.2f"), *distance);
                g_originalSaved = false;
                g_originalDistance = 0.0f;
            }

            // --- Speed (для всех) ---
            if (speedBonusEnabled) {
                float* speed = (float*)((unsigned char*)statFile + OFFSET_SPEED);
                if (!g_speedSaved) {
                    g_originalSpeed = *speed;
                    g_speedSaved = true;
                    LOGI(OBFUSCATE("[AttackDone] saved original speed = %.2f"), g_originalSpeed);
                }
                *speed = speedBonus;
                LOGI(OBFUSCATE("[AttackDone] speed = %.2f"), *speed);
            }
            else if (g_speedSaved) {
                float* speed = (float*)((unsigned char*)statFile + OFFSET_SPEED);
                *speed = g_originalSpeed;
                LOGI(OBFUSCATE("[AttackDone] restored speed = %.2f"), *speed);
                g_speedSaved = false;
                g_originalSpeed = 0.0f;
            }

            // --- Attack (для всех) ---
            if (attackBonusEnabled) {
                float* attack = (float*)((unsigned char*)statFile + OFFSET_ATTACK);
                if (!g_attackSaved) {
                    g_originalAttack = *attack;
                    g_attackSaved = true;
                    LOGI(OBFUSCATE("[AttackDone] saved original attack = %.2f"), g_originalAttack);
                }
                *attack = attackBonus;
                LOGI(OBFUSCATE("[AttackDone] attack = %.2f"), *attack);
            }
            else if (g_attackSaved) {
                float* attack = (float*)((unsigned char*)statFile + OFFSET_ATTACK);
                *attack = g_originalAttack;
                LOGI(OBFUSCATE("[AttackDone] restored attack = %.2f"), *attack);
                g_attackSaved = false;
                g_originalAttack = 0.0f;
            }
        }
    }
    old_AttackDone(__this, isRanged);
}

// ============================================================
//  Меню
// ============================================================
jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;
    const char *features[] = {
        OBFUSCATE("Category_Основное"),
        OBFUSCATE("100_Toggle_Режим бога"),
        OBFUSCATE("101_Toggle_Бонус монет"),
        OBFUSCATE("102_Toggle_Гарантия дропа"),
        OBFUSCATE("103_Toggle_Бонус Distance (+210)"),
        OBFUSCATE("104_Toggle_Бонус Speed (100.0)"),
        OBFUSCATE("105_Toggle_Бонус Attack (+1000)"),

        OBFUSCATE("Category_Бонусы опыта"),
        OBFUSCATE("200_Toggle_Level"),
        OBFUSCATE("201_Toggle_Combat"),
        OBFUSCATE("202_Toggle_Distance"),
        OBFUSCATE("203_Toggle_Magic"),

        OBFUSCATE("Collapse_Настройки бонусов"),
        OBFUSCATE("CollapseAdd_300_InputValue_1000000_Бонус монет"),
        OBFUSCATE("CollapseAdd_301_InputLValue_10000000_Бонус Level"),
        OBFUSCATE("CollapseAdd_302_InputLValue_100000000_Бонус Combat"),
        OBFUSCATE("CollapseAdd_303_InputLValue_100000000_Бонус Distance"),
        OBFUSCATE("CollapseAdd_304_InputLValue_100000000_Бонус Magic"),
    };

    int Total_Feature = (sizeof features / sizeof features[0]);
    ret = (jobjectArray)
            env->NewObjectArray(Total_Feature, env->FindClass(OBFUSCATE("java/lang/String")),
                                env->NewStringUTF(""));
    for (int i = 0; i < Total_Feature; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));
    return ret;
}

void Changes(JNIEnv *env, jclass clazz, jobject obj,
             jint featNum, jstring featName,
             jint value, jlong Lvalue, jboolean boolean, jstring text) {
    switch (featNum) {
        case 100: godMode          = boolean; break;
        case 101: coinMultEnabled  = boolean; break;
        case 102: forceDropEnabled = boolean; break;
        case 103: distanceBonusEnabled = boolean; break;
        case 104: speedBonusEnabled    = boolean; break;
        case 105: attackBonusEnabled   = boolean; break;

        case 200: expLevelEnabled    = boolean; break;
        case 201: expCombatEnabled   = boolean; break;
        case 202: expDistanceEnabled = boolean; break;
        case 203: expMagicEnabled    = boolean; break;

        case 300: coinMult = (value > 0) ? value : 0; break;
        case 301: multLevel    = (double)Lvalue; break;
        case 302: multCombat   = (double)Lvalue; break;
        case 303: multDistance = (double)Lvalue; break;
        case 304: multMagic    = (double)Lvalue; break;
    }
	#if defined(__aarch64__)
            Toast(env, obj, "arm64-v8a is connected", 0);
	#elif defined(__arm__)
            Toast(env, obj, "armeabi-v7a is connected", 0);
	#endif
}

// ============================================================
//  Поток установки хуков
// ============================================================
#define targetLibName OBFUSCATE("libil2cpp.so")

void *hack_thread(void *) {
    LOGI(OBFUSCATE("pthread created"));

    ElfScanner g_il2cppELF;
    do {
        sleep(1);
        g_il2cppELF = ElfScanner::createWithPath(targetLibName);
    } while (!g_il2cppELF.isValid());

    LOGI(OBFUSCATE("%s has been loaded"), (const char*)targetLibName);

#if defined(__aarch64__)
    // --- Stat.AddExp ---
    // Класс: Stat | Функция: AddExp(long exp)
    HOOK(targetLibName, str2Offset(OBFUSCATE("0x186DD88")), Hook_AddExp, old_AddExp);

    // --- Hero.GetDmg ---
    // Класс: Hero | Функция: GetDmg(int, int, int*) -> bool
    HOOK(targetLibName, str2Offset(OBFUSCATE("0x184C5B0")), Hook_GetDmg, old_GetDmg);

    // --- CombatObject.ApplyHp ---
    // Класс: CombatObject | Функция: ApplyHp(int, bool)
    HOOK(targetLibName, str2Offset(OBFUSCATE("0x1845FC4")), Hook_ApplyHp, old_ApplyHp);

    // --- Coin.ShowCoin ---
    // Класс: Coin | Функция: ShowCoin(float, void*, void*, bool, void*)
    HOOK(targetLibName, str2Offset(OBFUSCATE("0x188A148")), Hook_ShowCoin, old_ShowCoin);

    // --- LootManager.CreateLoot ---
    // Класс: LootManager | Функция: CreateLoot(Enemy)
    HOOK(targetLibName, str2Offset(OBFUSCATE("0x1851EF4")), Hook_CreateLoot, old_CreateLoot);

    // --- Enemy.DrawItems ---
    // Класс: Enemy | Функция: DrawItems() -> List<ItemData>
    HOOK(targetLibName, str2Offset(OBFUSCATE("0x1849B30")), Hook_DrawItems, old_DrawItems);

    // --- LootManager.ItemHasReachBag ---
    // Класс: LootManager | Функция: ItemHasReachBag(ItemData)
    HOOK(targetLibName, str2Offset(OBFUSCATE("0x1852BA0")), Hook_ItemHasReachBag, old_ItemHasReachBag);

    // --- LootItem.Activation ---
    // Класс: LootItem | Функция: Activation(ItemData, int, float, float, Vector3*, float)
    HOOK(targetLibName, str2Offset(OBFUSCATE("0x1869C0C")), Hook_LootItemActivation, old_LootItemActivation);

    // --- HeroStats.AttackDone ---
    // Класс: HeroStats | Функция: AttackDone(bool isRanged)
    HOOK(targetLibName, str2Offset(OBFUSCATE("0x186DA4C")), Hook_AttackDone, old_AttackDone);
#elif defined(__arm__)
    // ARMv7 — при необходимости
	    // --- Stat.AddExp ---
    // Класс: Stat | Функция: AddExp(long exp)
    HOOK(targetLibName, str2Offset(OBFUSCATE("0xD1FEB8")), Hook_AddExp, old_AddExp);

    // --- Hero.GetDmg ---
    // Класс: Hero | Функция: GetDmg(int, int, int*) -> bool
    HOOK(targetLibName, str2Offset(OBFUSCATE("0xCF6774")), Hook_GetDmg, old_GetDmg);

    // --- CombatObject.ApplyHp ---
    // Класс: CombatObject | Функция: ApplyHp(int, bool)
    HOOK(targetLibName, str2Offset(OBFUSCATE("0xCEE8F4")), Hook_ApplyHp, old_ApplyHp);

    // --- Coin.ShowCoin ---
    // Класс: Coin | Функция: ShowCoin(float, void*, void*, bool, void*)
    HOOK(targetLibName, str2Offset(OBFUSCATE("0xD42E48")), Hook_ShowCoin, old_ShowCoin);

    // --- LootManager.CreateLoot ---
    // Класс: LootManager | Функция: CreateLoot(Enemy)
    HOOK(targetLibName, str2Offset(OBFUSCATE("0xCFD604")), Hook_CreateLoot, old_CreateLoot);

    // --- Enemy.DrawItems ---
    // Класс: Enemy | Функция: DrawItems() -> List<ItemData>
    HOOK(targetLibName, str2Offset(OBFUSCATE("0xCF32F0")), Hook_DrawItems, old_DrawItems);

    // --- LootManager.ItemHasReachBag ---
    // Класс: LootManager | Функция: ItemHasReachBag(ItemData)
    HOOK(targetLibName, str2Offset(OBFUSCATE("0xCFE534")), Hook_ItemHasReachBag, old_ItemHasReachBag);

    // --- LootItem.Activation ---
    // Класс: LootItem | Функция: Activation(ItemData, int, float, float, Vector3*, float)
    HOOK(targetLibName, str2Offset(OBFUSCATE("0xD1AF24")), Hook_LootItemActivation, old_LootItemActivation);

    // --- HeroStats.AttackDone ---
    // Класс: HeroStats | Функция: AttackDone(bool isRanged)
    HOOK(targetLibName, str2Offset(OBFUSCATE("0xD1FAF4")), Hook_AttackDone, old_AttackDone);
#endif

    LOGI(OBFUSCATE("Done"));
    return nullptr;
}

__attribute__((constructor))
void lib_main() {
    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}