# Sidewalk location viewer

A local map of the positions sent by the [Sidewalk location sample](../../samples/sid_location).
It runs on your laptop and serves the map only on `127.0.0.1`. Nothing is hosted or published.

```
device -> AWS IoT Wireless positioning -> position destination -> IoT rule (republish)
       -> MQTT topic sidewalk/location-demo/... -> viewer.py (SigV4) -> browser on 127.0.0.1
```

On start, the viewer reads the last known position of each Sidewalk device with `GetResourcePosition`.
Then it subscribes to the MQTT topic that the rule republishes to and adds every new position to the map.
History is kept in memory only.

## One-time AWS setup (administrator)

1. Create the republish role: an IAM role that AWS IoT can assume (`iot.amazonaws.com`), with `iot:Publish` on `arn:aws:iot:REGION:ACCOUNT_ID:topic/sidewalk/location-demo/*`.
2. Create the rule from [topic-rule.json](topic-rule.json), after replacing `ACCOUNT_ID`:

   ```
   aws iot create-topic-rule --rule-name SidewalkLocationDemo --topic-rule-payload file://topic-rule.json
   ```

   The position destination invokes the rule by name.
3. Create the position destination that uses the rule.
   Its role must trust `iotwireless.amazonaws.com` and allow `iot:DescribeEndpoint` and `iot:Publish`:

   ```
   aws iotwireless create-destination --name SidewalkLocationDemo \
       --expression-type RuleName --expression SidewalkLocationDemo \
       --role-arn arn:aws:iam::ACCOUNT_ID:role/SidewalkLocationDemoDestination
   ```

4. Create a permission set in IAM Identity Center with the inline policy from [iam-policy.json](iam-policy.json), after replacing `REGION` and `ACCOUNT_ID`.
   The policy is read-only: it lists devices, reads the positions of the demo devices, and subscribes to the demo topic.
   Assign it to the users who run the demo.

Each device must be added with positioning enabled and this destination selected, see the sample documentation.
Also add the tag `demo` with the value `sidewalk-location-demo` to each demo device.
The policy allows reading the position and the configuration only of the devices with this tag, so the viewer cannot read the positions of other devices in the account.
The list of devices is not limited by the tag, so the viewer can see the names and ids of all devices, but not their positions.

To check what the rule publishes, subscribe to `sidewalk/location-demo/#` in the MQTT test client of the AWS IoT console.

## Running the viewer

```
pip install -r requirements.txt
aws configure sso          # once, creates a profile, for example "sidewalk-demo"
aws sso login --profile sidewalk-demo
python3 viewer.py --profile sidewalk-demo
```

Open http://127.0.0.1:8000/.

Options:

* `--device ID` shows only the given wireless device id; repeat it for more devices.
  By default, the viewer shows the devices whose position the policy allows reading, that is the devices with the demo tag.
* `--topic` sets the MQTT topic filter, by default `sidewalk/location-demo/#`.
* `--endpoint` sets the AWS IoT data endpoint, by default it is looked up with `DescribeEndpoint`.
* `--port` sets the local port, by default `8000`.

The marker color shows the method reported in the message: blue for BLE, green for Wi-Fi, orange for GNSS, gray if the message does not name a method.
The circle around the newest marker shows the horizontal accuracy.

## Security

* Credentials are temporary credentials from AWS IAM Identity Center (`aws sso login`).
  There are no access keys in this directory, in a file, or in the page.
* The page is served only on `127.0.0.1` and only to requests with a `127.0.0.1` or `localhost` host name.
* The only requests that leave the laptop, apart from AWS, are map tiles from `tile.openstreetmap.org`.
  They show which map area is open, but not the device data.
* [Leaflet](https://leafletjs.com/) 1.9.4 is stored in `static/leaflet` (BSD-2-Clause, see `static/leaflet/LICENSE`), so no script is loaded from a CDN.
