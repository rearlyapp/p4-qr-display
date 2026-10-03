#pragma once

// Copy this file to secrets.h. Never commit secrets.h.
#define STOCKIN_WIFI_SSID       "your-2.4GHz-ssid"
#define STOCKIN_WIFI_PASSWORD   "your-password"
// Optional additional known network; leave empty to disable.
#define STOCKIN_EXTRA_WIFI_SSID ""
#define STOCKIN_EXTRA_WIFI_PASSWORD ""

// Warehouse routes used by the working Android Ante Room flow.
#define WAREHOUSE_LABELS_URL       "http://77.42.39.77:8000/wms/labels"
#define WAREHOUSE_ANTE_STOCKIN_URL "http://77.42.39.77:8000/packing/ante-stock-in"

// Read-only packing-session metadata route used by the Supervisor QR example.
#define PACKING_API_ORIGIN       "https://65.108.56.155"
#define PACKING_API_ROUTE_HOST   "marine.rearlytech.com"
