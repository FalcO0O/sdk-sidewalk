.. _sidewalk_location:

Sidewalk location
#################

.. contents::
   :local:
   :depth: 2

This sample sends the device position to AWS over Amazon Sidewalk.
Every minute, it runs the Sidewalk location library.
On the nRF54L15 TAG, it uses Bluetooth® LE.
On the nRF54LM20 DK, it uses managed effort: Bluetooth LE first, then Wi-Fi, then GNSS if the previous method failed.
A button press sends the position at once with a chosen method.
Between location runs, the device sleeps.

The sample has no shell and no bootloader.
The same firmware image is used for all devices of one board type, and the device credentials are added when the device is programmed.

Requirements
************

.. list-table::
   :header-rows: 1

   * - Hardware platform
     - Build target
     - Location methods
     - Sidewalk links
   * - nRF54L15 TAG (`nrf54l15tag`_)
     - ``nrf54l15tag/nrf54l15/cpuapp``
     - Bluetooth LE (L1)
     - Bluetooth LE
   * - `nRF54LM20 DK`_ with the :ref:`nRF Sidewalk EB <nrf_sidewalk_eb>`
     - | ``nrf54lm20dk/nrf54lm20a/cpuapp``
       | ``nrf54lm20dk/nrf54lm20b/cpuapp``
     - Bluetooth LE (L1), Wi-Fi (L3), GNSS (L4)
     - Bluetooth LE and LoRa

You also need:

* A Sidewalk gateway in range, for example an `Echo 4th Gen`_.
* An AWS account with AWS IoT Core for Amazon Sidewalk, see :ref:`setting_up_sidewalk_prototype`.
* For the nRF54L15 TAG, a development kit with a **DEBUG OUT** connector to program it, for example the `nRF54L15 DK`_.
* For current measurements, the `Power Profiler Kit II`_.

LoRa is used only as a link on the nRF54LM20 DK.
The sample does not use LoRa location (L2) or FSK.
For details on the location levels, see :ref:`location_services` and the `Amazon Sidewalk Location Library Developer Guide`_.

Overview
********

The application runs one thread with a `Zephyr State Machine Framework`_ state machine.
The thread waits on a message queue, so the system goes to the deepest System ON sleep state between events.
Timers, buttons, and Sidewalk events wake it up.

.. code-block:: none

   boot ──► Connecting ──► Locating ◄──► Idle
                              │
                              └─► reboot (run timeout)

Boot
   The application initializes the Sidewalk platform and checks the manufacturing data.
   If there is no manufacturing data, the red LED blinks once and the application stops.
   Otherwise, it calls ``sid_init()`` and ``sid_start()``, and sets the Bluetooth LE auto connect policy.

Connecting
   The application waits until Sidewalk is ready, registered, and time synced.
   Then it initializes the location library and starts the first location run.
   The location library is initialized only after Sidewalk is ready, as described in :ref:`known_issues`.

Locating
   One location run is in progress.
   When the run is done, the LED shows which method worked, and the application goes to **Idle**.
   If a periodic run fails, the application runs it again, up to ``CONFIG_SID_LOCATION_MAX_ATTEMPTS`` times.
   If a run does not finish within ``CONFIG_SID_LOCATION_RUN_TIMEOUT_S``, the device reboots.
   This recovers from a stuck LR1110 scan.

Idle
   The device sleeps until the periodic timer expires or a button is pressed.

Location runs
=============

The location library is initialized as follows:

* nRF54L15 TAG: Bluetooth LE only, maximum effort L1, without managed effort.
  Periodic runs use L1.
* nRF54LM20 DK with the nRF Sidewalk EB: all methods, maximum effort L4, with managed effort.
  The library pauses and resumes the Sidewalk links during scans, and sends the Wi-Fi and GNSS results over Bluetooth LE or LoRa.

An L1 run sends no scan data, so it does not trigger the Bluetooth LE auto connect.
If the Bluetooth LE link is down, the application sets the connection request in the Sidewalk beacon with ``sid_ble_bcn_connection_request()``, and starts the L1 run when the gateway connects.

On the nRF54LM20 DK, periodic runs use ``SID_LOCATION_EFFORT_DEFAULT``.
The library tracks the level: it starts at L1 (Bluetooth LE), and raises the level to L3 (Wi-Fi) and then L4 (GNSS) after a failed run.
One call to ``sid_location_run()`` is one attempt, so after a failure the application calls it again.
It retries only while the level reported in the result goes up, so the same method is not repeated, and it stops after the highest method of the board.
After a successful L3 or L4 run, stepdown timers of 10 seconds bring the level back toward L1, so the next periodic run starts with Bluetooth LE again.

A Wi-Fi run counts as successful when the scan is sent.
If the cloud cannot resolve the position from it, the device is not notified, and GNSS is not tried.

Button runs use a fixed level, and restart the periodic timer.
After a run with a fixed level, the library can set the managed level outside the range L1 to L4 and then rejects the next ``SID_LOCATION_EFFORT_DEFAULT`` run.
In that case, the application initializes the location library again and repeats the run.

User interface
==============

.. list-table::
   :header-rows: 1

   * - Control
     - nRF54L15 TAG
     - nRF54LM20 DK
   * - Bluetooth LE location now
     - Button (P0.00)
     - **Button 0**
   * - Wi-Fi location now
     - --
     - **Button 1**
   * - GNSS location now
     - --
     - **Button 2**

**Button 3** on the nRF54LM20 DK does nothing.
A button press during a running location run is ignored.

After each run, an LED blinks once:

.. list-table::
   :header-rows: 1

   * - Result
     - nRF54L15 TAG
     - nRF54LM20 DK
   * - Position sent with Bluetooth LE
     - Blue
     - **LED 0**
   * - Position sent with Wi-Fi
     - --
     - **LED 1**
   * - Position sent with GNSS
     - --
     - **LED 2**
   * - Run failed, or no manufacturing data
     - Red
     - **LED 3**

Configuration
*************

.. list-table::
   :header-rows: 1

   * - Kconfig option
     - Default
     - Description
   * - ``CONFIG_SID_LOCATION_INTERVAL_S``
     - 60
     - Time between periodic location runs, in seconds.
   * - ``CONFIG_SID_LOCATION_RUN_TIMEOUT_S``
     - 180
     - Time after which an unfinished run reboots the device, in seconds.
   * - ``CONFIG_SID_LOCATION_MAX_ATTEMPTS``
     - 3
     - Maximum number of runs for one periodic location, one for each method.

The sample has the following configuration files:

* :file:`prj.conf` -- Debug configuration, with logs.
* :file:`prj_release.conf` -- Release configuration, without logs, console, serial, or asserts.
  Use it for the demo and for current measurements.

Building
********

Build the firmware once for each board type.
Run the commands from the west workspace directory.

nRF54L15 TAG, release:

.. code-block:: console

   west build -p -b nrf54l15tag/nrf54l15/cpuapp sidewalk/samples/sid_location -d build_tag -- -DFILE_SUFFIX=release

nRF54L15 TAG, debug with logs over RTT:

.. code-block:: console

   west build -p -b nrf54l15tag/nrf54l15/cpuapp sidewalk/samples/sid_location -d build_tag -S rtt-console

The TAG has no UART, so the debug build uses the ``rtt-console`` snippet.
It needs the SEGGER module.
If your west workspace uses the Sidewalk manifest only, the module is not included: add ``segger`` to the ``name-allowlist`` in :file:`west.yml`, or add the module path with ``-DEXTRA_ZEPHYR_MODULES``.

nRF54LM20 DK with the nRF Sidewalk EB, release:

.. code-block:: console

   west build -p -b nrf54lm20dk/nrf54lm20a/cpuapp sidewalk/samples/sid_location -d build_lm20 -- -DSHIELD=nrf_sidewalk_eb -DFILE_SUFFIX=release

For the debug build, leave out ``-DFILE_SUFFIX=release``.
Logs are on the first UART of the DK.

AWS setup
*********

Do these steps once for the demo account:

1. Create the position destination and the IoT rule that republishes positions to an internal MQTT topic.
   The steps and the policy files are in :file:`tools/location_demo/README.md`.
2. Create a Sidewalk device profile, if you do not have one.

For each device, in the AWS IoT console:

1. Go to :guilabel:`Manage → LPWAN devices → Devices` and add a Sidewalk device with the device profile.
2. Set :guilabel:`Positioning` to :guilabel:`Enabled`, and select the position destination from step 1.
3. Download the device JSON file (:file:`certificate.json`).

The device JSON file contains the device keys.
Keep it private, and do not commit it to a repository.

To create many devices, use the `AWS CLI`_ instead:

.. code-block:: console

   aws iotwireless create-wireless-device --type Sidewalk --name TAG-07 \
       --destination-name <uplink destination> --positioning Enabled \
       --sidewalk '{"DeviceProfileId": "<profile id>", "Positioning": {"DestinationName": "SidewalkLocationDemo"}}'
   aws iotwireless get-wireless-device --identifier-type WirelessDeviceId \
       --identifier <device id> > TAG-07_wireless_device.json
   aws iotwireless get-device-profile --id <profile id> > device_profile.json

Then pass the files with ``-W`` and ``-D`` instead of ``-C`` in the next step.

Provisioning and programming
****************************

Program the application, and then write the device credentials to the ``mfg_storage`` partition:

.. code-block:: console

   west flash -d build_tag
   west sid provision -d build_tag -C certificate.json

For details, see :ref:`setting_up_sidewalk_prototype`.

To program the nRF54L15 TAG, connect it to the **DEBUG OUT** connector of a development kit.
If more than one board is connected, the commands ask which one to program, or you can pass the serial number of the board.

.. caution::
   ``west flash --erase`` and a full chip erase delete the manufacturing data and the Sidewalk registration.
   After that, provision the device again.

After programming, keep the device near the gateway until it registers and gets time sync.
The first location run starts right after that.

Viewing the positions
*********************

Use the local viewer in :file:`tools/location_demo`.
It reads the last position of each device, subscribes to the MQTT topic of the position rule, and shows the positions on a map at ``http://127.0.0.1:8000``.
It uses temporary credentials from ``aws sso login`` and a read-only permission set.
See :file:`tools/location_demo/README.md`.

For debugging, subscribe to ``sidewalk/location-demo/#`` in the `AWS IoT MQTT client`_.

Current consumption
*******************

Between runs, the CPU sleeps in System ON, with RAM retained and the GRTC running.
System OFF is not used: every periodic wake-up would then mean a full boot, Sidewalk initialization, and a new time sync.
The sleep current depends mostly on the radio:

* The release configuration has no logs, console, or UART.
* On the nRF54L15 TAG, the unused sensors, their buses, and the ADC are disabled.
* On the nRF54LM20 DK, LoRa runs in the asynchronous profile, and the LR1110 sleeps between scans.
  The nRF Sidewalk EB adds several hundred µA through its level shifters, so treat it as a demo platform.
  For possible hardware changes, see the sleep current section in :ref:`nrf_sidewalk_eb`.
* Bluetooth LE advertising between runs is the main remaining load.
  To try a slower advertising interval, set ``CONFIG_SIDEWALK_BLE_ADV_INT_SLOW``, and check that the device still connects for each run.
  The interval is set in Kconfig, because changing it at runtime with ``SID_OPTION_BLE_USER_CONFIG`` is not supported (see :ref:`known_issues`).

To measure the current with the Power Profiler Kit II:

1. Build the release configuration.
2. Program and provision the device, and wait until it registers and the first run is done.
3. Supply the device from the PPK2 in source meter mode, at 3.0 V for the nRF54L15 TAG.
   On the nRF54LM20 DK, measure on the **VDD_EXT** pin as described for the nRF Sidewalk EB.
   Disconnect the debugger, because an active debug session keeps the device awake.
4. Record at least three periodic runs.
5. Note the average current between runs, the charge of one run for each method, and the average current over one interval.

Write the results here before the demo:

.. list-table::
   :header-rows: 1

   * - Board and configuration
     - Sleep current
     - Bluetooth LE run
     - Wi-Fi run
     - GNSS run
     - Average over 60 s
   * - nRF54L15 TAG, release
     - TBD
     - TBD
     - --
     - --
     - TBD
   * - nRF54LM20 DK and EB, release
     - TBD
     - TBD
     - TBD
     - TBD
     - TBD

Testing on hardware
*******************

Check the following on hardware before a demo:

* Managed effort with both links on the nRF54LM20 DK: a periodic run indoors goes from Bluetooth LE to Wi-Fi, and outdoors to GNSS.
* Button runs with a fixed level work while managed effort is enabled.
* After a Wi-Fi or GNSS run, the next periodic run starts with Bluetooth LE again.
* Wi-Fi and GNSS results go over LoRa when there is no Bluetooth LE connection.
* No LR1110 errors (busy timeouts, stale IRQ status) appear in the debug log during repeated scans.
* ``GetResourcePosition`` and the rule message include the method and the accuracy, and the viewer shows them.
* The current measurements in `Current consumption`_.

Troubleshooting
***************

The red LED blinks once at boot and nothing else happens
   The device has no manufacturing data.
   Program it with ``west sid provision``.

No location run after programming
   The device waits for registration and time sync.
   Keep it near the gateway, and check that the gateway has Sidewalk enabled.
   In the debug build, the log shows the registration and time sync state.

Every run fails with the red LED
   On the nRF54L15 TAG, Bluetooth LE location needs a gateway in range.
   On the nRF54LM20 DK, Wi-Fi needs visible access points and GNSS needs open sky.
   Check that positioning is enabled for the device in AWS.

The device reboots during a run
   The run did not finish within ``CONFIG_SID_LOCATION_RUN_TIMEOUT_S``.
   This is the recovery for a stuck scan.
   If it happens often, check the debug log.

The viewer shows no new positions
   Check the rule in the `AWS IoT MQTT client`_, and check that the device uses the position destination.
   If ``aws sso login`` expired, log in again and restart the viewer.

Limitations
***********

* No firmware update over the air: the sample has no bootloader.
* The method shown by the LED is the method that the library reports, not the accuracy of the position.

.. toctree::
   :maxdepth: 1
   :caption: Subpages:

   sid_location_demo_card.rst
