/************************************** OTA *****************************************/
const int FW_VERSION = 1;                                                        // Version number, don't forget to update this on changes
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
// Note the raw.githubuserconent, this allows us to access the contents at the url, not the webpage itself
const char* fwURLBase = "https://raw.githubusercontent.com/BIUS-USherbrooke/BIUS_Domotique/ESP_EAU/Modules/ESP_EAU/ESP_EAU"; // IP adress to the subfolder containing the binary and version number for this specific device

/************************************** CAYENNE *****************************************/
#define CAYENNE_PRINT Serial               
#include <CayenneMQTTESP8266.h>

// If too many errors when connecting, deep sleep (which will restart the esp)
// PIN D0 OF THE NODEMCU MUST BE CONNECTed TO THE RST PIN
int failedConnections = 0;
#define SLEEP_TIME 20e6

// WiFi network info.
char ssid[] = "MotoCoco";
char wifiPassword[] = "VG360esp";

// Cayenne authentication info. This should be obtained from the Cayenne Dashboard.
char username[] = "75b6d920-cb5b-11e8-8a08-61f9e5c4f140";
char password[] = "13a3664856850223f1994846cd5aa6d668226da9";
char clientID[] = "dd26c560-1904-11e9-b82d-f12a91579eed";

// Channels cayenne
#define VIRTUAL_CHANNEL 98                                                       // Version number
#define VIRTUAL_CHANNEL 0                                                       // Channel to force an OTA update

#define VIRTUAL_CHANNEL 1                                                       // Solenoide 1
#define VIRTUAL_CHANNEL 2                                                       // Solenoide 2

#define VIRTUAL_CHANNEL 4                                                       // Float switch resevoir superieur
#define VIRTUAL_CHANNEL 5                                                       // Float switch reservoir principal (bas)
#define VIRTUAL_CHANNEL 6                                                       // Niveau d'eau (ultrason) reservoir principal

#define VIRTUAL_CHANNEL 7                                                       // Secondes a continuer le pompage vers le reservervoir superieur apres avoir atteint la switch
#define VIRTUAL_CHANNEL 8                                                       // Secondes a ouvrir solenoide 1
#define VIRTUAL_CHANNEL 9                                                       // Secondes a ouvrir solenoide 2

/*********************************Donnees********************************************/
// Temps entre les prises de donnees (en secondes)
#define DELAIS_PRISE_DONNEES 10 // Secondes
unsigned long lastMillis = 0;   // Dernière prise de mesure

// Float sensors
int floatSuspendu = D1;
int floatReservoirPrincipal = D2;

// Water level sensor stuff
int depth = 37; //Enter depth of your tank here in centimeters
int trig = D5; // Attach Trig of ultrasonic sensor to pin D5
int echo = D6; // Attach Echo of ultrasonic sensor to pin D6

// Solenoides
int solenoide1 = D8;
int solenoide1OpenDuration = 1; // Temps pour ouvrir solenoide 1
unsigned long solenoide1OpenTime = 0;   // Temps auquel le solenoide 1 a ete ouvert
bool solenoide1Open = false;
int solenoide2 = D7;
int solenoide2OpenDuration = 1; // Temps pour ouvrir solenoide 2
unsigned long solenoide2OpenTime = 0;   // Temps auquel le solenoide 2 a ete ouvert
bool solenoide2Open = false;

// Pompe
int pinPompe = D4;
int tempsDePompage = 1;               // Duree de pompage (secondes) apres avoir activer la switch
bool pumpingState = false;            // Savoir si ca pompe ou pas
unsigned long floatSuspenduActivation = 0;    // Le temps que le to pa ete trigger
bool floatPumpingActivated = false;   // Savoir si on est en processus de l'arreter

int luminositePin = A0;

void setup() {
  Serial.begin(9600);
  delay(10);

  // Ultrason
  pinMode(trig, OUTPUT);
  pinMode(echo, INPUT);
  // Solenoide 1 relais
  pinMode(solenoide1, OUTPUT);
  digitalWrite(solenoide1, HIGH);
  // Solenoide 2 relais
  pinMode(solenoide2, OUTPUT);
  digitalWrite(solenoide2, HIGH);
  // Pompe relais
  pinMode(pinPompe, OUTPUT);
  digitalWrite(pinPompe, HIGH);
  // Float sensors
  pinMode(floatSuspendu, INPUT);
  pinMode(floatReservoirPrincipal, INPUT);
     
  Cayenne.begin(username, password, clientID, ssid, wifiPassword);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    Cayenne.loop();

    // S'il y a eu assez de temps, republier les données
    if (millis() - lastMillis > DELAIS_PRISE_DONNEES*1000) {
      lastMillis = millis();

      checkWaterLevelUltrason();  // Check Water level and send to cayenne
      checkWaterLevelSuspendu();  // Check Water level suspendu

      Cayenne.virtualWrite(98,FW_VERSION);                                         // Send the version number to Cayenne
    }
  } else {
    failedConnections++;
    // Si pas connecté plus de 10 fois, sleep pour SLEEP TIME et réveil après
    if (failedConnections > 10) {
      Serial.println("No wifi connection, going to sleep before restarting.");
      ESP.deepSleep(SLEEP_TIME);
    } else {
      // Sinon, delay 1000
      delay(1000);
    }
  }
  checkWaterLevelSuspendu();  // Check Water level suspendu, pas dans les autres boucles au cas ou on aurait un debordement
  checkSolenoide1();
  checkSolenoide2();
}

/***************************************** Cayenne Ins **********************************************/
// Check for updates on web server
CAYENNE_IN(0)
{
  checkForUpdates();
}

// Modifier le temps de surpompage
CAYENNE_IN(7)
{
  tempsDePompage = getValue.asInt();
}

// Modifier le temps douverture pour solenoide1
CAYENNE_IN(8)
{
  solenoide1OpenDuration = getValue.asInt();
}

// Modifier le temps douverture pour solenoide2
CAYENNE_IN(9)
{
  solenoide2OpenDuration = getValue.asInt();
}

// Ouvrir solenoide 1
CAYENNE_IN(1)
{
  digitalWrite(solenoide1, LOW);
  solenoide1OpenTime = millis();
  solenoide1Open = true;
}

// Ouvrir solenoide 2
CAYENNE_IN(2)
{
  digitalWrite(solenoide2, LOW);
  solenoide2OpenTime = millis();
  solenoide2Open = true;
}
/***************************************** http update function *************************************/
// This function checks the web server to see if a new version number is available, if so, it updates with the new firmware (binary)
void checkForUpdates() {
  String fwImageURL = String(fwURLBase);
      fwImageURL.concat( ".ino.nodemcu.bin" );                                              // Adds the url for the binary
      Serial.println(fwImageURL);
      t_httpUpdate_return ret = ESPhttpUpdate.update( fwImageURL , "", "CC:AA:48:48:66:46:0E:91:53:2C:9C:7C:23:2A:B1:74:4D:29:9D:33");             // Update the esp with the new binary, third is the certificate of the site
      delay(50);

      switch(ret) {
        case HTTP_UPDATE_FAILED:
          Serial.printf("HTTP_UPDATE_FAILED Error (%d): %s", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
          break;

        case HTTP_UPDATE_NO_UPDATES:
          Serial.println("no updates");
          break;
  }
}

/******************************************** Water level (ultrason) ************************************/
void checkWaterLevelUltrason() {
  // Establish variables for duration of the ping,
  long duration, cm;       

  // The PING is triggered by a HIGH pulse of 2 or more microseconds.
  // Give a short LOW pulse beforehand to ensure a clean HIGH pulse:
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);

  // The same pin is used to read the signal from the PING: a HIGH
  // pulse whose duration is the time (in microseconds) from the sending
  // of the ping to the reception of its echo off of an object.
  duration = pulseIn(echo, HIGH);

  // Convert the time into a distance
  cm = depth - microsecondsToCentimeters(duration);

  Cayenne.virtualWrite(6, cm);  // Envoi hauteur de la tank
  Cayenne.virtualWrite(4, digitalRead(floatSuspendu));  // Envoi etat floatSuspendu
  Cayenne.virtualWrite(5, digitalRead(floatReservoirPrincipal));  // Envoi etat floatReservoirPrincipal
}

long microsecondsToCentimeters(long microseconds)
{
  // The speed of sound is 340 m/s or 29 microseconds per centimeter.
  // The ping travels out and back, so to find the distance of the
  // object we take half of the distance travelled.
  return microseconds / 29 / 2;
}

/********************************************** Water level reservoir suspendu ******************************/
void checkWaterLevelSuspendu() {
  // Si le float switch en haut n'est pas a LOW et qu'il n'est pas entrain de pomper, commence à pomper
  if (digitalRead(floatSuspendu) && !pumpingState) {
    // Verifie qu'il a assez d'eau dans le bac du bas avant de commencer à pomper
    if (!digitalRead(floatReservoirPrincipal)) {
      pumpingState = true;
      digitalWrite(pinPompe, LOW);      // Activer le relais pour pomper
    }
  } else if (!digitalRead(floatSuspendu) && pumpingState) {
    // Si le float switch est a LOW et que c'est entrain de pomper, attends le temps specifié et arrete de pomper
    // Si on a pas active le warning, active le
    if (!floatPumpingActivated) {
      // Sinon, active le warning qu'on a détecté et prends le temps de début
      floatSuspenduActivation = millis();
      floatPumpingActivated = true;
    } else if (millis() - floatSuspenduActivation >= tempsDePompage*1000){
      // sinon, si ca fait assez longtemps qu'on pompe, arrete le pompage pi reset les bools
      digitalWrite(pinPompe, HIGH);
      pumpingState = false;
      floatPumpingActivated = false;
    }
    delay(floatSuspenduActivation*1000);
  }
}

/************************************************** solenoides ***************************************************/
void checkSolenoide1() {
  // Si solenoide 1 a ete ouvert depuis assez longtemps ET i lest ouvert, le fermer
  if (millis() - solenoide1OpenTime >= solenoide1OpenDuration*1000 && solenoide1Open) {
    digitalWrite(solenoide1, HIGH);
    solenoide1Open = false;
  }
}

void checkSolenoide2() {
  // Si solenoide 2 a ete ouvert depuis assez longtemps ET il est ouvert, le fermer
  if (millis() - solenoide2OpenTime >= solenoide2OpenDuration*1000 && solenoide2Open) {
    digitalWrite(solenoide2, HIGH);
    solenoide2Open = false;
  }
}
