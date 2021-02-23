// 
// BLE Central Program for M5Stack
// author: N.Fukuoka
// license: MIT
//
#include "FS.h"
#include "SPIFFS.h"
#include <FunctionFSM.h>
#include <NimBLEDevice.h>
#include <M5Stack.h>
#include <vector>
#include <M5TreeView.h>

#define BC_DEVINFO_FILE "/BC_SAVEDATA.dat"
#define BC_DEV_NOUSE "NO USE"
#define BC_NAME_STR_LEN (8)
#define BC_BD_NAME_LEN (32)
#define BC_BD_ADDRESS_LEN (17)
#define BC_MAX_NOTIFY_COUNT (10)

#define BC_IDX_INIT (999)
#define BC_IDX_NOUSE (990)

enum BcBdKind {
  BC_BD_CSC = 0,
  BC_BD_HID,
  BC_BD_NUM
};
enum BcBdState {
  BC_STS_INIT = 0,
  BC_STS_NOUSE,
  BC_STS_OFF,
  BC_STS_ON,
  BC_STS_NOTIFY,
  BC_STS_NUM
};

// save data
typedef char BC_bd_name_t[BC_BD_NAME_LEN];
typedef char BC_bd_address_t[BC_BD_ADDRESS_LEN];
struct BcDevInfo {
  NimBLEUUID      servUUID;
  NimBLEUUID      charUUID;
  char            message[BC_NAME_STR_LEN];
  BcBdState       sts;
  BC_bd_address_t address;
  uint8_t         dmy[1];
  BC_bd_name_t    name;
};
struct BcSaveFileInfo {
  BcDevInfo dev[BC_BD_NUM];
};
BcSaveFileInfo savedata = {};
BcSaveFileInfo initdata = {};

BcBdState prevSts[BC_STS_NUM] = {};
int notifyCount[BC_STS_NUM] = {};
int prevCount[BC_STS_NUM] = {};

struct BcIndicator {
  int16_t   x;
  int16_t   y;
  int16_t   w;
  int16_t   h;
  uint16_t  fill_col;
  uint16_t  line_col;
};

typedef std::vector<MenuItem*> vmi;
static NimBLEUUID serv_cscUUID("1816");  // UUID CSC Cycling Speed and Cadence
static NimBLEUUID char_cscUUID("2a5b");  // UUID CSC Report Charcteristic
static NimBLEUUID serv_hidUUID("1812");  // UUID HID Human Interface Device
static NimBLEUUID char_hidUUID("2a4d");  // UUID HID Report Charcteristic

// UUID CSC Report Charcteristic data
typedef struct _ST_WHEEL_DATA { // Wheel Revolution Data
  uint32_t count;     // Cumulative Wheel Revolutions
  uint16_t lasttime;  // Last Event Time (Unit:1/1024s)
} ST_WHEEL_DATA;
typedef struct _ST_CRANK_DATA { // Crank Revolution Data
  uint16_t count;     // Cumulative Crank Revolutions
  uint16_t lasttime;  // Last Event Time (Unit:1/1024s)
} ST_CRANK_DATA;


static NimBLEAdvertisedDevice *advDevice;
std::vector<NimBLEAdvertisedDevice*> advDevices;
int scanDevice;
M5TreeView treeView;
M5TreeView initTreeView = {};
int devIdx = 0;

// ---- util ----
boolean check_address( BC_bd_address_t a1, NimBLEAddress a2) {
  //Serial.printf("check_address: a1[%s] a2[%s]\n", a1, a2.toString().c_str());
  if (0==memcmp(a1, a2.toString().c_str(), sizeof(BC_bd_address_t))) {
    return true;
  }
  return false;
}

boolean is_allstate( BcBdState s1) {

  for (int bd=0; bd<BC_BD_NUM; bd++) {
    if (savedata.dev[bd].sts != BC_STS_NOUSE && savedata.dev[bd].sts != s1) {
          return false;
    }
  }
  return true;
}

void set_persistent_data(BcSaveFileInfo* dat) {
    strncpy(dat->dev[BC_BD_CSC].message, "CSC", sizeof(dat->dev[BC_BD_CSC].message));
    dat->dev[BC_BD_CSC].servUUID = serv_cscUUID;
    dat->dev[BC_BD_CSC].charUUID = char_cscUUID;
    strncpy(dat->dev[BC_BD_HID].message, "HID", sizeof(dat->dev[BC_BD_HID].message));
    dat->dev[BC_BD_HID].servUUID = serv_hidUUID;
    dat->dev[BC_BD_HID].charUUID = char_hidUUID;
}

int BcSaveConfig(BcSaveFileInfo *buf, int size) {
  int ret = -9;
  int err = 0;
  File fd = SPIFFS.open(BC_DEVINFO_FILE, "wb");
  if (fd) {
    if ( (err = fd.write((byte*)buf, size)) == size) {
      ret = 0;
    } else {
      Serial.printf(" fd.write() error=%d\n", err);
      ret = -1;
    }
    fd.close();
  } else {
    ret = -2;
  }
  return ret;
}
int BcReadConfig(BcSaveFileInfo *buf, int size) {
  int ret = -9;
  int err = 0;
   
  File fd = SPIFFS.open(BC_DEVINFO_FILE, "rb");
  if (fd) {
    if ( (err = fd.read((byte*)buf, size)) == size) {
      ret = 0;
    } else {
      Serial.printf(" fd.read() error=%d\n", err);
      ret = -1;
    }
    fd.close();
    
  } else {
    Serial.printf("%s open error.\n", BC_DEVINFO_FILE);
    ret = -2;
  }
  int i=0;
  return ret;
}

void displayTitle(String txt, uint16_t size, uint16_t color, uint16_t bgcol = TFT_BLACK){
  M5.Lcd.fillRect(0,0,320,40,bgcol);
  M5.Lcd.setFreeFont(&FreeSans9pt7b);
  M5.Lcd.setTextColor(color,bgcol);
  M5.Lcd.setTextSize(size);
  M5.Lcd.drawString(txt, 2, 2);
}
void displayIndicator(BcBdKind bd, BcBdState sts){

#define BC_IW (60)

  BcIndicator a[BC_BD_NUM] ={{ 4,  80, BC_IW, 12, 0, 0},   /*CSC*/
                             { 4, 140, BC_IW, 12, 0, 0}};  /*HID*/
                             
  BcIndicator b[BC_STS_NUM] ={{ 0, 0, 0, 0, 0, 0},  /*INIT*/
                              { 0, 0, 0, 0, TFT_DARKGREY, TFT_DARKGREY},  /*NOUSE*/
                              { 0, 0, 0, 0, TFT_DARKGREY, TFT_WHITE},  /*OFF*/
                              { 0, 0, 0, 0, TFT_GREEN, TFT_WHITE},  /*ON*/
                              { 0, 0, 0, 0, TFT_DARKGREEN, TFT_WHITE}}; /*NOTIFY*/

  M5.Lcd.fillRect(a[bd].x,a[bd].y,a[bd].w,a[bd].h,b[sts].fill_col);
  if (notifyCount[bd] > 0) {
    int16_t w = BC_IW / BC_MAX_NOTIFY_COUNT;
    int16_t x = w * (notifyCount[bd]-1) + 4;
    M5.Lcd.fillRect(x,a[bd].y,w,a[bd].h,TFT_DARKGREEN);
  }
  M5.Lcd.drawRect(a[bd].x,a[bd].y,a[bd].w,a[bd].h,b[sts].line_col);
}
void displayDevice(BcDevInfo *dev, BcBdKind bd, BcBdState sts){
  
  BcIndicator a[BC_BD_NUM] ={{ 80,  60, 290, 50, 0, 0},   /*CSC*/
                             { 80, 120, 290, 50, 0, 0}};  /*HID*/
  BcIndicator b[BC_BD_NUM] ={{  4,  60,  60, 24, 0, 0},   /*CSC*/
                             {  4, 120,  60, 24, 0, 0}};  /*HID*/

  uint16_t color = TFT_WHITE;
  if (sts == BC_STS_NOUSE) color = TFT_DARKGREY;
  
  M5.Lcd.setFreeFont(&FreeSans9pt7b);
  M5.Lcd.setTextColor(color,TFT_BLACK);
  M5.Lcd.setTextSize(1);
  
  M5.Lcd.fillRect(b[bd].x,b[bd].y,b[bd].w,b[bd].h,TFT_BLACK);
  M5.Lcd.drawString(String(dev->message), b[bd].x, b[bd].y);

  M5.Lcd.fillRect(a[bd].x,a[bd].y,a[bd].w,a[bd].h,TFT_BLACK);
  M5.Lcd.drawString(String(dev->name), a[bd].x, a[bd].y);
  M5.Lcd.drawString(String(dev->address), a[bd].x, a[bd].y + 25);
}
void updateScreen(bool force = false){
  BcDevInfo *dev;

  for (int bd=0; bd<BC_BD_NUM; bd++) {
    if (force || prevSts[bd] != savedata.dev[bd].sts) {
      dev = &(savedata.dev[bd]);
      displayDevice(dev, (BcBdKind)bd, dev->sts);
      displayIndicator((BcBdKind)bd, dev->sts);

      prevSts[bd] = savedata.dev[bd].sts;
    }
  }
}
void updateIndicator(bool force = false){
  BcDevInfo *dev; 

  for (int bd=0; bd<BC_BD_NUM; bd++) {
    if (force || prevSts[bd] != savedata.dev[bd].sts || prevCount[bd] != notifyCount[bd]) {
      dev = &(savedata.dev[bd]);
      displayIndicator((BcBdKind)bd, dev->sts);

      prevSts[bd] = savedata.dev[bd].sts;
      prevCount[bd] = notifyCount[bd];
    }
  }
}

// ---- Callback ----
void f0_scanEndedCB(NimBLEScanResults results) {
  Serial.println("Scan Ended");
}
class f0_AdvertisedDeviceCallbacks: public NimBLEAdvertisedDeviceCallbacks {
    
    void onResult(NimBLEAdvertisedDevice *advertisedDevice) {
        NimBLEUUID  serviceUUID = savedata.dev[scanDevice].servUUID;
        
        Serial.print("Advertised Device found: ");
        Serial.printf("name:%s,address:%s ", advertisedDevice->getName().c_str(), advertisedDevice->getAddress().toString().c_str());
        Serial.printf("UUID:%s\n", advertisedDevice->haveServiceUUID() ? advertisedDevice->getServiceUUID().toString().c_str() : "none");

        if (advertisedDevice->isAdvertisingService(serviceUUID)) {
            Serial.print("Found Our Service: ");
            Serial.println(serviceUUID.toString().c_str());
            
            for (int bd=0; bd<BC_BD_NUM; bd++) {
              
              if (check_address(savedata.dev[bd].address, advertisedDevice->getAddress())) {
                  Serial.printf("onResult:device skip.\n");
              } else {
                  if (advDevices.size() == 0) {
                    advDevice = advertisedDevice;
                    advDevices.push_back(advertisedDevice);
                  }
                  else {
                    for (int i = 0; i < advDevices.size(); i++) {
                      if (advDevices.at(i)->getAddress().equals(advertisedDevice->getAddress())) {
                        Serial.printf("onResult:device already added\n");
                        return;
                      }
                    }
                    advDevice = advertisedDevice;
                    advDevices.push_back(advertisedDevice);
                  }
                  Serial.printf("onResult:myDevices=%d\n", advDevices.size());
              }
            }
        }
    }
    ;
};


void dummyFunc(MenuItem* mi) {
  Serial.printf("callback Item= <%s> Tag=%d\n",  mi->title.c_str(), mi->tag);
}

class f0_ClientCallbacks: public NimBLEClientCallbacks {
  
  void onConnect(NimBLEClient *pClient) {
    Serial.println("Connected");
  }
  ;

  void onDisconnect(NimBLEClient *pClient) {
    Serial.print(pClient->getPeerAddress().toString().c_str());
    Serial.println(" Disconnected.");
  }
  ;

  bool onConnParamsUpdateRequest(NimBLEClient *pClient, const ble_gap_upd_params *params) {
      bool ret = true;
      
      // ex: PanoBike request = 640 960 0 600
      //   CW Shutter request =  16  32 2 300
      //
      if(params->itvl_min < 16) { /** 1.25ms units */
          ret = false;
      } else if(params->itvl_max > 1000) { /** 1.25ms units */
          ret = false;
      } else if(params->latency > 2) { /** Number of intervals allowed to skip */
          ret = false;
      } else if(params->supervision_timeout > 1000) { /** 10ms units */
          ret = false;
      }
      
      Serial.printf("-- onConnParamsUpdateRequest %s = %d  itvl %d %d lcy %d tout %d\n",
        pClient->getPeerAddress().toString().c_str(), ret, params->itvl_min,params->itvl_max,params->latency,params->supervision_timeout );
      return ret;
  }
  ;

  uint32_t onPassKeyRequest() {
    Serial.println("Client Passkey Request");
    /** return the passkey to send to the server */
    return 123456;
  }
  ;

  bool onConfirmPIN(uint32_t pass_key) {
    Serial.print("The passkey YES/NO number: ");
    Serial.println(pass_key);
    /** Return false if passkeys don't match. */
    return true;
  }
  ;

  void onAuthenticationComplete(ble_gap_conn_desc *desc) {
    if (!desc->sec_state.encrypted) {
      Serial.println("Encrypt connection failed - disconnecting");
      /** Find the client with the connection handle provided in desc */
      NimBLEDevice::getClientByID(desc->conn_handle)->disconnect();
      return;
    }
  }
  ;
};

boolean connectTest(NimBLEUUID serviceUUID, int adv_idx) {
    NimBLEClient *pClient = nullptr;
    Serial.printf("connectTest() serviceUUID = %s  adv_idx = %d\n", serviceUUID.toString().c_str(), adv_idx);
    boolean canNotify = false;

    if (!pClient) {
        if (NimBLEDevice::getClientListSize() >= NIMBLE_MAX_CONNECTIONS) {
            Serial.println("Max clients reached - no more connections available");
            return false;
        }

        pClient = NimBLEDevice::createClient();

        Serial.println("New client created");

        pClient->setClientCallbacks(new f0_ClientCallbacks(), false);
        pClient->setConnectionParams(12, 12, 0, 51);
        pClient->setConnectTimeout(10);

        if (!pClient->connect(advDevices.at(adv_idx))) {
            NimBLEDevice::deleteClient(pClient);
            Serial.println("Failed to connect.");
            return false;
        }
    }

    Serial.print("Connected to: ");
    Serial.print(pClient->getPeerAddress().toString().c_str());
    Serial.print("     RSSI: ");
    Serial.println(pClient->getRssi());

    NimBLERemoteService *pSvc = nullptr;
    //  NimBLERemoteCharacteristic *pChr = nullptr;
    std::vector<NimBLERemoteCharacteristic*> *pChrs = nullptr;

    pSvc = pClient->getService(serviceUUID);
    if (pSvc) { /** make sure it's not null */
        pChrs = pSvc->getCharacteristics(true);
    }

    if (pChrs) { /** make sure it's not null */

        for (int i = 0; i < pChrs->size(); i++) {
            Serial.printf("pChrs->at(%d)->canNotify() = %d\n", i, pChrs->at(i)->canNotify() );
            if (pChrs->at(i)->canNotify()) {
              Serial.printf("found Notify Characteristics\n");
              canNotify = true;
            }
        }
    } else {
        Serial.println("service not found.");
    }
    
    pClient->disconnect();
    Serial.println("Done with this device!");
    return canNotify;
}

//FSM CONFIG-------------------------------------------------------------------------
//fsm triggers
enum Trigger {
  NONE = 0,
  FREEZE,
  NEXT,
  LOOP,
  KEEPCONNECT
};
enum Trigger trigger;

// ***** fsm state functions (ent_*, act_*, ext_*) *****

// freeze ***********************************************************
void ent_freeze() {
  Serial.println("ent FREEZE");
}
void act_freeze() {
  delay(5000);
}

// init ***********************************************************
void ent_init() {
  Serial.println("ent INIT");
  int err = 0;
  NimBLEDevice::setScanFilterMode(CONFIG_BTDM_SCAN_DUPL_TYPE_DEVICE);
  NimBLEDevice::setScanDuplicateCacheSize(100);
  NimBLEDevice::init("");

  if (M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed()){
    // remove savedata
    if (SPIFFS.exists(BC_DEVINFO_FILE)) {
      SPIFFS.remove(BC_DEVINFO_FILE);
      delay(500);
      Serial.printf("remove %s\n", BC_DEVINFO_FILE);
    }
  }
}

void act_init() {
  Serial.printf("NimBLEDevice::getInitialized()=%d\n", NimBLEDevice::getInitialized());
  
  if (NimBLEDevice::getInitialized()) {
    //trigger global fsm transition
    trigger = NEXT;
  }
}

// checkfile **********************************************************
void ent_checkfile() {
  Serial.println("ent CHECKFILE");
  // init and read savedata
  int err = 0;
  savedata = initdata;
  if (SPIFFS.exists(BC_DEVINFO_FILE)) {
      if ( err = BcReadConfig(&savedata, sizeof(savedata) )) {
        Serial.printf("error: BcReadConfig() = %d\n", err);
      } else {
        for (int i=0; i<BC_BD_NUM; i++) {
          Serial.printf("read: dev[%d] --  servUUID : [%s] charUUID : [%s] ", i, savedata.dev[i].servUUID.toString().c_str(), savedata.dev[i].charUUID.toString().c_str());
          Serial.printf("sts=%d name=[%s] address=[%s]\n", savedata.dev[i].sts, savedata.dev[i].name, savedata.dev[i].address);
        }
      }
  }
}

void act_checkfile() {

  if (0==memcmp(&savedata, &initdata, sizeof(savedata))) {
    Serial.printf("savedata == initdata\n");
    set_persistent_data(&savedata);    
    scanDevice = BC_BD_CSC;
    trigger = NEXT;
  } else {
    Serial.printf("savedata != initdata\n");
    trigger = KEEPCONNECT;
  }
}

// scan **********************************************************
void ent_scan() {
  Serial.println("ent SCAN");
  Serial.printf("start SCAN scanDevice=[%s]\n", savedata.dev[scanDevice].message);

  M5.Lcd.fillScreen(TFT_BLACK);
  displayTitle("scan " + String(savedata.dev[scanDevice].message) + " device ...", 1, TFT_CYAN);
  
  advDevices.clear();
  
  NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_SC);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); /** +9db */
  /** create new scan */
  NimBLEScan *pScan = NimBLEDevice::getScan();
  pScan->setActiveScan(true);  // Need device_name, request SCAN_RSP.
  pScan->setDuplicateFilter(true);
  pScan->setInterval(5000);   // msec
  pScan->setWindow(5000);   // msec
  
  pScan->setAdvertisedDeviceCallbacks(new f0_AdvertisedDeviceCallbacks());
  bool err = pScan->start( 15, f0_scanEndedCB );
  Serial.printf("pScan->start() = %d\n", err);
}

void act_scan() {
  delay(100);
  if (!NimBLEDevice::getScan()->isScanning()) {
    trigger = NEXT;
  }
}

void ext_scan() {
  Serial.println("ext SCAN");
  NimBLEDevice::getScan()->stop();
  Serial.printf("advDevices.size()=%d\n", advDevices.size());
  M5.Lcd.fillScreen(TFT_BLACK);
}

// select **********************************************************
void ent_select() {
  Serial.println("ent SELECT");
  M5.Lcd.fillScreen(TFT_BLACK);
  displayTitle("select " + String(savedata.dev[scanDevice].message) + " device", 1, TFT_CYAN);

  treeView = initTreeView;
  
  treeView.clientRect.x = 0;
  treeView.clientRect.y = 48;
  treeView.clientRect.w = 318;
  treeView.clientRect.h = 170;
  treeView.itemWidth    = 318;
  treeView.fontColor[0]  = 0xFFFF;
  treeView.backColor[0]  = 0x0000;
  treeView.frameColor[0] = 0x0118;
  treeView.fontColor[1]  = 0xFFFF;
  treeView.backColor[1]  = 0x441F;
  treeView.frameColor[1] = 0xFFF0;
  treeView.backgroundColor = 0x0000;
  treeView.itemHeight = 22;
  treeView.setFreeFont(&FreeSans9pt7b);
  
  M5ButtonDrawer::setFreeFont(&FreeSans9pt7b);
  M5ButtonDrawer::height = 20;
  M5ButtonDrawer::width = 100;
  
  int i=0;  
  if (advDevices.size() > 0) {
    for (; i < advDevices.size(); i++) {
      std::string devname = advDevices.at(i)->getName() + " (" + advDevices.at(i)->getAddress().toString() + ")";
            
      treeView.addItem(new MenuItem(devname.c_str(), i, dummyFunc));
      Serial.printf("%d %s <%s>\n", i, advDevices.at(i)->getName().c_str(), advDevices.at(i)->getAddress().toString().c_str());
    }
  }
  // add no use Item
  treeView.addItem(new MenuItem(BC_DEV_NOUSE, i, dummyFunc));
  Serial.printf("%d %s <>\n", i, BC_DEV_NOUSE);
  
  treeView.begin();
  Serial.printf("    treeView.begin()\n");
}

void act_select() {
  MenuItem* mi = treeView.update();
  if (mi != NULL) {
    if (mi->tag < advDevices.size()) {
      devIdx = mi->tag;
      Serial.printf("%d %s %s\n", devIdx, advDevices.at(devIdx)->getName().c_str(), advDevices.at(devIdx)->getAddress().toString().c_str());
    } else {
      devIdx = BC_IDX_NOUSE;
      Serial.printf("%d %s\n", devIdx, BC_DEV_NOUSE);
    }

    Serial.printf("savedata.dev[%d] devIdx = %d\n", scanDevice, devIdx);
  
    if (devIdx != BC_IDX_INIT) {
      BcDevInfo devinfo = {};
      
      if (devIdx == BC_IDX_NOUSE) {
        strcpy(devinfo.name, BC_DEV_NOUSE);
        devinfo.sts = BC_STS_NOUSE;
      } else {
        strncpy(devinfo.address, advDevices.at(devIdx)->getAddress().toString().c_str(), sizeof(devinfo.address));
        strncpy(devinfo.name, advDevices.at(devIdx)->getName().c_str(), sizeof(devinfo.name));

        boolean err = connectTest(savedata.dev[scanDevice].servUUID, devIdx);
        if (err) {
          displayTitle("CONNECT OK", 2, TFT_GREEN);
          devinfo.sts = BC_STS_OFF;
        } else {
          displayTitle("CONNECT ERROR", 2, TFT_WHITE, TFT_RED);
          trigger = FREEZE;
          return;
        }
      }
      
      savedata.dev[scanDevice] = devinfo;
      Serial.printf("%d -- name=[%s] address=[%s] sts=%d\n", scanDevice, savedata.dev[scanDevice].name, savedata.dev[scanDevice].address, savedata.dev[scanDevice].sts);
    }  
  
    // next DevInfo
    scanDevice++;
    if (scanDevice < BC_BD_NUM ) {
      trigger = LOOP;
    } else {
      trigger = NEXT;
    }
  }  
}

void ext_select() {
  Serial.println("ext SELECT");
  delay(1000);
}

// saveconfig **********************************************************
void ent_saveconfig() {
  Serial.println("ent SAVECONFIG");
  set_persistent_data(&savedata);

  for (int i=0; i<BC_BD_NUM; i++) {
    Serial.printf("save: dev[%d] -> servUUID : [%s] charUUID : [%s] ", i, savedata.dev[i].servUUID.toString().c_str(), savedata.dev[i].charUUID.toString().c_str());
    Serial.printf("sts=%d name=[%s] address=[%s]\n", savedata.dev[i].sts, savedata.dev[i].name, savedata.dev[i].address);
  }
}
void act_saveconfig() {
  int err = 0;

  if ( err = BcSaveConfig(&savedata, sizeof(savedata) )) {
    Serial.printf("error: BcSaveConfig() = %d\n", err);
    trigger = FREEZE;
    return;
  }
  delay(500);
  trigger = NEXT;
}

// reboot **********************************************************
void ent_reboot() {
  Serial.println("ent REBOOT");
  NimBLEDevice::deinit(true);
  delay(1000);
  // reboot
  esp_restart();
}

// keepconnect **********************************************************

// ---- variables and callbacks for keepconnect
static bool doConnect = false;
static uint32_t scanTime = 0; /** 0 = scan forever */

void f1_scanEndedCB(NimBLEScanResults results) {
  Serial.println("--- Scan Ended ---");
}
class f1_ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) {
        Serial.print("Connected: ");
        Serial.println(pClient->toString().c_str());

        pClient->updateConnParams(48,48,0,120);
    };

    void onDisconnect(NimBLEClient* pClient) {
      
        for (int bd=0; bd<BC_BD_NUM; bd++) {
          if (check_address(savedata.dev[bd].address, pClient->getPeerAddress())) {
            savedata.dev[bd].sts=BC_STS_OFF;
            Serial.printf("************ Disconnected [%s] -- ", savedata.dev[bd].name );
            Serial.println(pClient->getPeerAddress().toString().c_str());
          }
        }

        Serial.println("************  Starting scan...");
        NimBLEDevice::getScan()->start(scanTime, f1_scanEndedCB);
    };

    bool onConnParamsUpdateRequest(NimBLEClient* pClient, const ble_gap_upd_params* params) {
        bool ret = true;
        
        // ex: PanoBike   request = 640 960 0 600
        //     CW Shutter request =  16  32 2 300
        //
        if(params->itvl_min < 16) { /** 1.25ms units */
            ret = false;
        } else if(params->itvl_max > 1000) { /** 1.25ms units */
            ret = false;
        } else if(params->latency > 2) { /** Number of intervals allowed to skip */
            ret = false;
        } else if(params->supervision_timeout > 1000) { /** 10ms units */
            ret = false;
        }
        Serial.printf("-- onConnParamsUpdateRequest %s = %d  itvl %d %d lcy %d tout %d\n",
          pClient->getPeerAddress().toString().c_str(), ret, params->itvl_min,params->itvl_max,params->latency,params->supervision_timeout );
 
        return ret;
    };

    uint32_t onPassKeyRequest(){
        Serial.println("Client Passkey Request");
        /** return the passkey to send to the server */
        return 123456;
    };

    bool onConfirmPIN(uint32_t pass_key){
        Serial.print("The passkey YES/NO number: ");
        Serial.println(pass_key);

        return true;
    };

    void onAuthenticationComplete(ble_gap_conn_desc* desc){
        if(!desc->sec_state.encrypted) {
            Serial.println("Encrypt connection failed - disconnecting");
            NimBLEDevice::getClientByID(desc->conn_handle)->disconnect();
            return;
        }
    };
};

static f1_ClientCallbacks clientCB;

class f1_AdvertisedDeviceCallbacks: public NimBLEAdvertisedDeviceCallbacks {

    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
      Serial.print("Advertised Device found: ");
      Serial.println(advertisedDevice->toString().c_str());

      for (int bd=0; bd<BC_BD_NUM; bd++) {
        if (check_address(savedata.dev[bd].address, advertisedDevice->getAddress())) {
            Serial.printf("Found Our Device [%s] [%s] [%s]\n", savedata.dev[bd].message, savedata.dev[bd].name, savedata.dev[bd].address);
            /** stop scan before connecting */
            NimBLEDevice::getScan()->stop();
            /** Save the device reference in a global for the client to use*/
            advDevice = advertisedDevice;
            /** Ready to connect now */
            doConnect = true;
        }
      }
    };
};

void notifyCB_hid(NimBLERemoteCharacteristic *pRemoteCharacteristic, uint8_t *pData, size_t length, bool isNotify) {
  std::string str = (isNotify == true) ? "Notification" : "Indication";
  str += " from ";
  str += pRemoteCharacteristic->getRemoteService()->getClient()->getPeerAddress().toString();
  str += ": Service = " + pRemoteCharacteristic->getRemoteService()->getUUID().toString();
  str += ", Characteristic = " + pRemoteCharacteristic->getUUID().toString();

  int xdata = 0;
  
  Serial.print(str.c_str());
  Serial.print("\ndata: ");
  for (int i = 0; i < length; i++) {
    Serial.printf("%02X ", pData[i]);
    xdata += pData[i];
  }
  Serial.print("   ");
  
  if ( xdata != 0) {
    // test
    Serial.printf("xdata:%x ", xdata);
    notifyCount[BC_BD_HID] = (notifyCount[BC_BD_HID] < BC_MAX_NOTIFY_COUNT ? notifyCount[BC_BD_HID] + 1 : 1);
    Serial.printf(" notifyCount=%d", notifyCount[BC_BD_HID]);
  }

  Serial.print("\n");
}
void notifyCB_csc(NimBLERemoteCharacteristic *pRemoteCharacteristic, uint8_t *pData, size_t length, bool isNotify) {
  std::string str = (isNotify == true) ? "Notification" : "Indication";
  str += " from ";
  str += pRemoteCharacteristic->getRemoteService()->getClient()->getPeerAddress().toString();
  str += ": Service = " + pRemoteCharacteristic->getRemoteService()->getUUID().toString();
  str += ", Characteristic = " + pRemoteCharacteristic->getUUID().toString();

  Serial.print(str.c_str());
  Serial.print("\ndata: ");
  for (int i = 0; i < length; i++) {
    Serial.printf("%02X ", pData[i]);
  }
  Serial.print("   ");
  
  if ((pData[0] & 0x01)&& 7<=length) {
    // Wheel Revolution Data
    ST_WHEEL_DATA *data = (ST_WHEEL_DATA*)&(pData[1]);
    Serial.printf("count:%u ", data->count);
    Serial.printf("lasttime:%hu ", data->lasttime);
    
  } else if ((pData[0] & 0x02) && 5<=length) {
    // Crank Revolution Data
    ST_CRANK_DATA *data = (ST_CRANK_DATA*)&(pData[1]);
    Serial.printf("count:%hu ", data->count);
    Serial.printf("lasttime:%hu ", data->lasttime);
    
  } else {
    Serial.print("unknown");
  }

  notifyCount[BC_BD_CSC] = (notifyCount[BC_BD_CSC] < BC_MAX_NOTIFY_COUNT ? notifyCount[BC_BD_CSC] + 1 : 1);
  Serial.printf(" notifyCount=%d\n", notifyCount[BC_BD_CSC]);
}

bool connectToServer() {
    NimBLEClient* pClient = nullptr;

    /** Check if we have a client we should reuse first **/
    if(NimBLEDevice::getClientListSize()) {

        pClient = NimBLEDevice::getClientByPeerAddress(advDevice->getAddress());
        if(pClient){
            if(!pClient->connect(advDevice, false)) {
                Serial.println("Reconnect failed");
                return false;
            }
            Serial.println("Reconnected client");
        } else {
            pClient = NimBLEDevice::getDisconnectedClient();
        }
    }

    /** No client to reuse? Create a new one. */
    if(!pClient) {
        if(NimBLEDevice::getClientListSize() >= NIMBLE_MAX_CONNECTIONS) {
            Serial.println("Max clients reached - no more connections available");
            return false;
        }

        pClient = NimBLEDevice::createClient();

        Serial.println("New client created");

        pClient->setClientCallbacks(&clientCB, false);

        pClient->setConnectionParams(12,12,0,51);
        pClient->setConnectTimeout(15);


        if (!pClient->connect(advDevice)) {
            NimBLEDevice::deleteClient(pClient);
            Serial.println("Failed to connect, deleted client");
            return false;
        }
    }

    if(!pClient->isConnected()) {
        if (!pClient->connect(advDevice)) {
            Serial.println("Failed to connect");
            return false;
        }
    }

    Serial.print("Connected to: ");
    Serial.println(pClient->getPeerAddress().toString().c_str());
    Serial.print("RSSI: ");
    Serial.println(pClient->getRssi());

    NimBLERemoteService* pSvc = nullptr;
    NimBLERemoteCharacteristic* pChr = nullptr;
    NimBLERemoteDescriptor* pDsc = nullptr;

    // --- HID 
    pSvc = pClient->getService(serv_hidUUID);
    if(pSvc) {
        Serial.printf("%s service found.\n", serv_hidUUID.toString().c_str());
        auto pCharacteristics = pSvc->getCharacteristics(true);
        for( pChr : *pCharacteristics ) { 
          if(pChr) {
              if(pChr->canNotify()) {
                  if(pChr->subscribe(true, notifyCB_hid)) {
                      savedata.dev[BC_BD_HID].sts = BC_STS_ON;
                      displayIndicator(BC_BD_HID, savedata.dev[BC_BD_HID].sts);
                      Serial.printf("Notify subscribed success** %s\n", pChr->toString().c_str());
                  } else {
                      pClient->disconnect();
                      return false;
                  }
              }
          } else {
            Serial.printf("%s characteristic not found.\n", char_hidUUID.toString().c_str());
          }
        }
    }

    // --- CSC 
    pSvc = pClient->getService(serv_cscUUID);
    if(pSvc) {
        Serial.printf("%s service found.\n", serv_cscUUID.toString().c_str());
        pChr = pSvc->getCharacteristic(char_cscUUID);
        if(pChr) {
            if(pChr->canNotify()) {
                if(pChr->subscribe(true, notifyCB_csc)) {
                    savedata.dev[BC_BD_CSC].sts = BC_STS_ON;
                    displayIndicator(BC_BD_CSC, savedata.dev[BC_BD_CSC].sts);
                    Serial.printf("Notify subscribed success** %s\n", pChr->toString().c_str());
                } else {
                    pClient->disconnect();
                    return false;
                }
            }
        } else {
          Serial.printf("%s characteristic not found.\n", char_cscUUID.toString().c_str());
        }
    }
    
    Serial.println("Done with this device!");
    return true;
}

// --------
void ent_keepconnect() {
  Serial.println("ent KEEPCONNECT");
  std::fill_n(prevSts, BC_BD_NUM, BC_STS_INIT);  // init update-screen members.
  std::fill_n(notifyCount, BC_BD_NUM, 0);       // init counter.

  M5.Lcd.fillScreen(TFT_BLACK);
  displayTitle("CONNECTING...", 2, TFT_CYAN);
  updateScreen(true);
  advDevices.clear();
  
  NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_SC);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); /** +9db */
  /** create new scan */
  NimBLEScan *pScan = NimBLEDevice::getScan();  
  pScan->setActiveScan(false);  // false=Passive Scan (No Need request SCAN_RSP.)
  pScan->setDuplicateFilter(true);

  pScan->setAdvertisedDeviceCallbacks(new f1_AdvertisedDeviceCallbacks());
  pScan->setInterval(45);   // msec
  pScan->setWindow(15);   // msec
  
  bool err = pScan->start( scanTime, f1_scanEndedCB);
  Serial.printf("pScan->start() = %d\n", err);
}

void act_keepconnect() {
    
    if(doConnect){

      doConnect = false;
      
      Serial.println("*1");
      if(connectToServer()) {
          Serial.println("Success! we should now be getting notifications, scanning for more!");
      } else {
          Serial.println("Failed to connect, starting scan");
      }
      
      Serial.println("*2");
      NimBLEDevice::getScan()->start(scanTime, f1_scanEndedCB);
      Serial.println("*3");
      
      if (is_allstate(BC_STS_ON)) {
        displayTitle("", 0, 0);
      }
      
    } else {
      updateIndicator();
      delay(1);
    }
}
  

//fsm states (&func_on_enter_state, &func_act_state, &func_on_exit_state)
FunctionState st_init(&ent_init, &act_init, nullptr);
FunctionState st_freeze(&ent_freeze, &act_freeze, nullptr);
FunctionState st_checkfile(&ent_checkfile, &act_checkfile, nullptr);
FunctionState st_scan(&ent_scan, &act_scan, &ext_scan);
FunctionState st_select(&ent_select, &act_select, &ext_select);
FunctionState st_saveconfig(&ent_saveconfig, &act_saveconfig, nullptr);
FunctionState st_reboot(&ent_reboot, nullptr, nullptr);
FunctionState st_keepconnect(&ent_keepconnect, &act_keepconnect, nullptr);

//fsm (&init_state)
FunctionFsm fsm(&st_init);

void initfsm() {
  fsm.add_transition(&st_init, &st_checkfile, NEXT, nullptr);
  
  fsm.add_transition(&st_checkfile, &st_keepconnect, KEEPCONNECT, nullptr);
  fsm.add_transition(&st_checkfile, &st_scan, NEXT, nullptr);
  
  fsm.add_transition(&st_scan, &st_freeze, FREEZE, nullptr);
  fsm.add_transition(&st_scan, &st_select, NEXT, nullptr);

  fsm.add_transition(&st_select, &st_scan, LOOP, nullptr);
  fsm.add_transition(&st_select, &st_saveconfig, NEXT, nullptr);
  fsm.add_transition(&st_select, &st_freeze, FREEZE, nullptr);
  
  fsm.add_transition(&st_saveconfig, &st_reboot, NEXT, nullptr);
  fsm.add_transition(&st_saveconfig, &st_freeze, FREEZE, nullptr);
  
}

//End of FSM CONFIG-------------------------------------------------------------------------

void setup() {
  Serial.println("setup");
  
  Serial.begin(115200);
  Serial.flush();
  M5.begin();
  M5.Lcd.begin();
  Wire.begin();
  SPIFFS.begin(true); /* format SPIFFS the first time */
  initfsm();
}

void loop() {
  M5.update();
  trigger = NONE;
  fsm.run_machine();
  if (trigger != NONE) fsm.trigger(trigger);
}
