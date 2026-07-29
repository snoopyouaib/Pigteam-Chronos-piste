#include "ImuManager.h"
#include <Arduino.h>
#include <math.h>
#include "i2c_bsp.h"

// ===================== Registres QMI8658 (bas niveau, I2C partage) =====================
//
// Mapping croise depuis la datasheet publique QMI8658C (QST/Waveshare)
// et plusieurs implementations open-source (tinygo/x/drivers/qmi8658c,
// lewisxhe/SensorLib) -- cf. commentaire d'en-tete de ImuManager.h pour
// la mise en garde sur la validation a faire sur ce board precis.
#define QMI8658_ADDR_LOW   0x6A
#define QMI8658_ADDR_HIGH  0x6B
#define QMI8658_REG_WHO_AM_I  0x00
#define QMI8658_WHO_AM_I_VAL  0x05
#define QMI8658_REG_CTRL1     0x02
#define QMI8658_REG_CTRL2     0x03 // accelerometre : bits[5:4] range, bits[3:0] ODR
#define QMI8658_REG_CTRL3     0x04 // gyroscope     : bits[6:4] range, bits[3:0] ODR
#define QMI8658_REG_CTRL7     0x08 // bit0 = aEN (accelerometre), bit1 = gEN (gyroscope)
#define QMI8658_REG_ACCEL_XL  0x35 // 6 octets : AX_L,AX_H,AY_L,AY_H,AZ_L,AZ_H
#define QMI8658_REG_GYRO_XL   0x3B // 6 octets : GX_L,GX_H,GY_L,GY_H,GZ_L,GZ_H

#define QMI8658_CTRL2_ACC_8G       0x20
#define QMI8658_CTRL2_ODR_1000HZ   0x03
#define QMI8658_CTRL3_GYRO_512DPS  0x40 // (512DPS = index 4 des 8 plages, cf. commentaire ci-dessous)
#define QMI8658_CTRL3_ODR_1000HZ   0x03

// Sensibilite -- LSB par unite, deduite de la plage configuree
// ci-dessus (16 bits signes sur +/- la pleine echelle) :
//   accelerometre +/-8g   -> 32768/8   = 4096 LSB/g   -> 1000/4096 mg par LSB
//   gyroscope     +/-512dps -> 32768/512 = 64 LSB/dps -> 1/64 dps par LSB
static const float ACCEL_MG_PER_LSB = 1000.0f / 4096.0f;
static const float GYRO_DPS_PER_LSB = 1.0f / 64.0f;

static uint8_t imuAddr = QMI8658_ADDR_LOW;
bool imuOk = false;

static float leanAngleDeg = 0.0f;
static unsigned long lastImuTickMs = 0;

static float lastAccelXmg = 0.0f, lastAccelYmg = 0.0f, lastAccelZmg = 0.0f;
static float lastGyroXdps = 0.0f, lastGyroYdps = 0.0f, lastGyroZdps = 0.0f;

// Frequence max d'echantillonnage/integration -- inutile de solliciter
// l'I2C partage (tactile + IMU) plus vite que ca pour un angle destine
// au log, pas a un affichage temps reel.
static const unsigned long IMU_TICK_MIN_INTERVAL_MS = 20; // ~50Hz

// Filtre complementaire : poids donne a l'integration gyro (tres
// precise a court terme, derive lentement) vs l'angle brut mesure par
// l'accelerometre seul (jamais de derive, mais fausse en virage a
// cause de l'acceleration laterale qui s'ajoute a la gravite). 0.98 =
// 98% gyro/2% accelero -- recale la derive du gyro sur plusieurs
// secondes sans etre perturbe par un virage de quelques secondes.
static const float COMPLEMENTARY_ALPHA = 0.98f;

static int16_t rawWordLE(uint8_t lo, uint8_t hi) {
  return (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

bool initImu() {
  uint8_t whoAmI = 0;

  for (uint8_t candidate : { (uint8_t)QMI8658_ADDR_LOW, (uint8_t)QMI8658_ADDR_HIGH }) {
    if (I2C_read_buff(candidate, QMI8658_REG_WHO_AM_I, &whoAmI, 1) == 0 && whoAmI == QMI8658_WHO_AM_I_VAL) {
      imuAddr = candidate;
      imuOk = true;
      break;
    }
  }
  if (!imuOk) return false;

  uint8_t ctrl2 = QMI8658_CTRL2_ACC_8G | QMI8658_CTRL2_ODR_1000HZ;
  uint8_t ctrl3 = QMI8658_CTRL3_GYRO_512DPS | QMI8658_CTRL3_ODR_1000HZ;
  uint8_t ctrl7 = 0x03; // aEN (bit0) + gEN (bit1)
  I2C_write_buff(imuAddr, QMI8658_REG_CTRL2, &ctrl2, 1);
  I2C_write_buff(imuAddr, QMI8658_REG_CTRL3, &ctrl3, 1);
  I2C_write_buff(imuAddr, QMI8658_REG_CTRL7, &ctrl7, 1);

  lastImuTickMs = millis();
  Serial.printf("IMU QMI8658 : adresse 0x%02X, WHO_AM_I=0x%02X (attendu 0x05).\n", imuAddr, whoAmI);
  return true;
}

void imuTick() {
  if (!imuOk) return;
  unsigned long nowMs = millis();
  if (nowMs - lastImuTickMs < IMU_TICK_MIN_INTERVAL_MS) return;
  float dtS = (nowMs - lastImuTickMs) / 1000.0f;
  lastImuTickMs = nowMs;
  if (dtS > 0.5f) return; // 1er appel ou reveil apres une longue pause -- dt pas fiable, saute ce tick sans casser l'angle courant

  uint8_t buf[6];
  if (I2C_read_buff(imuAddr, QMI8658_REG_ACCEL_XL, buf, 6) != 0) return;
  float ax = rawWordLE(buf[0], buf[1]) * ACCEL_MG_PER_LSB;
  float ay = rawWordLE(buf[2], buf[3]) * ACCEL_MG_PER_LSB;
  float az = rawWordLE(buf[4], buf[5]) * ACCEL_MG_PER_LSB;

  if (I2C_read_buff(imuAddr, QMI8658_REG_GYRO_XL, buf, 6) != 0) return;
  float gx = rawWordLE(buf[0], buf[1]) * GYRO_DPS_PER_LSB;
  float gy = rawWordLE(buf[2], buf[3]) * GYRO_DPS_PER_LSB;
  float gz = rawWordLE(buf[4], buf[5]) * GYRO_DPS_PER_LSB;

  lastAccelXmg = ax; lastAccelYmg = ay; lastAccelZmg = az;
  lastGyroXdps = gx; lastGyroYdps = gy; lastGyroZdps = gz;

  // ----- Mapping d'axe A CONFIRMER une fois monte dans le carenage -----
  // Hypothese de travail (a verifier physiquement, cf. commentaire en
  // tete de ImuManager.h) : le roulis (roll) se lit sur le gyro X et
  // se recale sur l'angle atan2(accelY, accelZ). Si le montage final
  // donne un axe different, ajuster gyroRollDps/accelRollDeg
  // ci-dessous -- rien d'autre a changer, le reste du fichier est
  // independant du mapping physique.
  float gyroRollDps = gx;
  float accelRollDeg = atan2f(ay, az) * 180.0f / (float)M_PI;

  leanAngleDeg = COMPLEMENTARY_ALPHA * (leanAngleDeg + gyroRollDps * dtS)
               + (1.0f - COMPLEMENTARY_ALPHA) * accelRollDeg;
}

float getLeanAngleDeg() {
  return imuOk ? leanAngleDeg : 0.0f;
}

void getImuRaw(float& accelXmg, float& accelYmg, float& accelZmg,
               float& gyroXdps, float& gyroYdps, float& gyroZdps) {
  accelXmg = lastAccelXmg; accelYmg = lastAccelYmg; accelZmg = lastAccelZmg;
  gyroXdps = lastGyroXdps; gyroYdps = lastGyroYdps; gyroZdps = lastGyroZdps;
}
