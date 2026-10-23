.. _sidewalk_location_demo_card:

Sidewalk location demo card
###########################

A one-page guide for running the :ref:`sidewalk_location` demo with customers.

What it shows
*************

* A small tag finds its position over Amazon Sidewalk, with no SIM card, no Wi-Fi password, and no phone.
* Every minute, the tag sends its position through a nearby Sidewalk gateway, such as an Echo device.
* The position appears on a map on your laptop.
* The development kit with the nRF Sidewalk EB adds Wi-Fi and GNSS positioning, and the LoRa link for longer range.
* Between positions, the device sleeps.

What you need
*************

* A provisioned nRF54L15 TAG, or an nRF54LM20 DK with the nRF Sidewalk EB, or both.
* A Sidewalk gateway nearby, for example an Echo device with Sidewalk enabled, in a country where Sidewalk is available.
* A laptop with the viewer from :file:`tools/location_demo`, and your AWS SSO login.

Before the meeting
******************

1. Log in: ``aws sso login --profile sidewalk-demo``.
2. Start the viewer: ``python3 viewer.py --profile sidewalk-demo``, and open ``http://127.0.0.1:8000``.
3. Power on the devices near the gateway, and wait until the first position appears on the map.

Running the demo
****************

1. Show the map with the last position of each device.
2. Press the tag button.
   The LED blinks blue when the position is sent, and the map updates after a short time.
3. On the development kit, press **Button 1** for Wi-Fi or **Button 2** for GNSS.
   The LED shows which method worked: **LED 0** Bluetooth LE, **LED 1** Wi-Fi, **LED 2** GNSS.
   GNSS works only with open sky, for example near a window or outside.
4. Explain the automatic mode: every minute, the device tries the method with the lowest power first and moves to a stronger method only when needed.

Key messages
************

* Sidewalk is a free, shared network made of Amazon devices that customers already own.
* The device is provisioned once and needs no pairing or app on site.
* Bluetooth LE location costs the least energy.
  Wi-Fi and GNSS are used only when Bluetooth LE is not enough.
* Nordic provides the radio SoC, the Sidewalk Add-on for the nRF Connect SDK, and this location sample.

If something does not work
**************************

* One red blink at power-on and no reaction to buttons: the device is not provisioned.
* No new positions: check that the gateway is on and has Sidewalk enabled, then press the button again.
* The map is not loading: log in again with ``aws sso login`` and restart the viewer.

Privacy and security
********************

* The map runs only on your laptop, nothing is published.
* The viewer has read-only access to the demo devices.
* Map tiles come from OpenStreetMap, which sees the map area you open, but not the device data.
