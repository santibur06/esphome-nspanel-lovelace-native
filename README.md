# ESPHome Native Component for NSPanel: layout editor fork

> **TFT uploads are fixed here.** If `upload_tft` stalls after the first block or leaves your screen on "System data ERROR!", this fork finishes the upload. It has the fixes from upstream [#77](https://github.com/olicooper/esphome-nspanel-lovelace-native/pull/77) and [#82](https://github.com/olicooper/esphome-nspanel-lovelace-native/pull/82), which aren't merged upstream yet. See [Installation](#installation).

This is a fork of [olicooper/esphome-nspanel-lovelace-native](https://github.com/olicooper/esphome-nspanel-lovelace-native). It adds what the [NSPanel Lovelace layout editor](https://github.com/santibur06/nspanel-lovelace-editor) needs to change a panel from Home Assistant, without compiling or flashing:

- [Runtime layouts](#runtime-layouts): the screensaver and cards are sent to the panel and stored there.
- [Panel settings](#panel-settings): a settings page on the panel, and a PIN lock that the editor can share across every panel.
- [Brightness rules](#brightness-rules) by time or from an entity.
- [Buttons and relays](#buttons-and-relays): the layout decides what each physical button does.
- [Screen light](#screen-light): the screen's brightness as Home Assistant lights.
- [TFT uploads that finish](#installation): the fixes from upstream [#77](https://github.com/olicooper/esphome-nspanel-lovelace-native/pull/77) and [#82](https://github.com/olicooper/esphome-nspanel-lovelace-native/pull/82), which aren't merged upstream yet.

Everything works with the same HMI TFT as upstream, described in [Installation](#installation). The features are on the `generic/runtime-layouts` branch, so point `external_components` at it, as the [basic](basic-example.yaml) and [advanced](advanced-example.yaml) examples do:

```yaml
external_components:
  - source: github://santibur06/esphome-nspanel-lovelace-native@generic/runtime-layouts
    components: [nspanel_lovelace]
```

The rest of this page is the upstream documentation, with sections added for these features.

## About the upstream project

This project is designed to utilise the UI provided by [NSPanel Lovelace UI](https://github.com/joBr99/nspanel-lovelace-ui), but use a native [ESPHome](https://github.com/esphome/esphome) component for the backend. My core goals for the project were to:
- Reduce bandwidth consumption - by utilising the efficient TCP communication implemented by ESPHome.
- Remove the need for an MQTT broker and make the device able to function on a minimal level during network outages.
- Make the screen more responsive by having the ESP32 do some of the heavy lifting (the ESP32 is actually quite powerful!)

A special thanks goes to these great projects for making this possible:
- [NSPanel Lovelace UI](https://github.com/joBr99/nspanel-lovelace-ui)
- [ESPHome NSPanel Lovelace UI](https://github.com/sairon/esphome-nspanel-lovelace-ui)
- [ESPHome](https://github.com/esphome/esphome)

# Installation

This library only works with the latest minor versions of ESPHome, older versions are not officially supported but may still work.

The installation of ESPHome on the ESP32 follows the standard ESPHome build method e.g. `esphome run --device COM6 basic-example.yaml`. It is possible to do OTA updates with the ESPHome CLI after the initial upload.

For the GUI, this project relies on the HMI TFT firmware from the [ESPHome NSPanel Lovelace UI](https://github.com/sairon/esphome-nspanel-lovelace-ui) project. 
The firmware version reported by the screen needs to be `52` or `53` for it to be compatible with this project.
If you don't have the custom HMI TFT firmware installed already you will need to upload the appropriate `nspanel*.tft` file from `v4.3.3` of the project which can be [found here](https://github.com/joBr99/nspanel-lovelace-ui/tree/v4.3.3/HMI). You can upload the correct TFT firmware after installing this component on the ESP32 by using the `upload_tft` service (seen in the [basic example](basic-example.yaml#L64)) exposed by the device in HomeAssistant. Pass it the file's raw URL, for example `https://raw.githubusercontent.com/joBr99/nspanel-lovelace-ui/v4.3.3/HMI/nspanel.tft`.

This fork's TFT upload has the fixes from upstream [#77](https://github.com/olicooper/esphome-nspanel-lovelace-native/pull/77) and [#82](https://github.com/olicooper/esphome-nspanel-lovelace-native/pull/82), which aren't merged upstream yet. Uploads that stall after the first block, or leave the screen on "System data ERROR!", finish here. After the last block the device waits for the display to restart before rebooting itself, which can take up to 30 seconds.

# Configuration

A basic configuration can be found in the [basic example](basic-example.yaml), but you'll probably also want to look at the [advanced example](advanced-example.yaml) which shows the features currently available. This is loosely based on the appdaemon configuration format ([found here](https://github.com/joBr99/nspanel-lovelace-ui/blob/v4.3.3/appdaemon/apps-simple.yaml)) to make it easier to transition to this native ESPHome solution, but please don't expect this to translate exactly as it is not possible to make it work in the exact same way given the limitations of the ESP32.

## PSRAM

The NSPanel has on-board PSRAM which this project makes use of automatically, which means that the `psram` component is unavailable because the PSRAM is configured during code generation by adding specific `sdkconfig_options`. Additionally, when using Arduino (which is deprecated) the PSRAM cannot be used as it is not possible to customise the PSRAM pins for the Arduino framework build.

### Icons

- Icon values can be an icon name or hex value (e.g. `hex:E549`). A list of icons can be found here: https://docs.nspanel.pky.eu/icon-cheatsheet.html
- Icon colours need to be a 16-bit number (0-65535) representing a `rgb565` colour code. This website can be used to select your colour https://chrishewett.com/blog/true-rgb565-colour-picker/ - then you take the rgb565 hex value and convert it to a number (e.g. `#ce79` -> `52857`).


## Runtime layouts

Add `runtime_layout: true` to change the screensaver and cards without compiling or flashing:

```yaml
api:
  custom_services: true

nspanel_lovelace:
  runtime_layout: true
```

The device then has a `set_layout` action that takes a JSON layout. It validates the layout, stores it in flash and reboots to apply it, replacing the `screensaver` and `cards` from your configuration. An empty value removes the stored layout, and the device goes back to its configuration. If a stored layout can't be used, the device also falls back to its configuration.

The device reports what it's using with the `esphome.nspanel_lovelace_layout_status` event, so "Allow the device to perform Home Assistant actions" must be on in its ESPHome integration settings. Call `report_layout_status` to ask for it again. The event also carries the lock state and the relay entities described below.

The [NSPanel Lovelace Layout Editor](https://github.com/santibur06/nspanel-lovelace-editor) lets you edit layouts from a sidebar page in Home Assistant. This option requires ESPHome 2025.7.0 or newer.

## Panel settings

Every panel gets a settings page unless you set `panel_settings: false`. It works without Home Assistant and has these rows:

- **Lock buttons** stops the physical buttons, no PIN involved. The relays still respond to the screen and Home Assistant.
- **Brightness** sets the screen's normal brightness.
- **Screensaver brightness** sets the brightness on the screensaver, from 0 to 100.
- **Turn screen off** turns the screen off until it's tapped. It needs a screensaver, and models with 4 rows leave it out.
- **Lock screen** asks for confirmation, then shows a keypad until the PIN is entered. This row only appears once a PIN has been set from Home Assistant.

With one page on the panel, settings opens from a gear in the right arrow slot. With more pages, the arrows at the ends of the rotation lead to it. On the settings page, the left button goes home and the right one opens a diagnostics page with the Wi-Fi signal, IP address, Home Assistant connection, uptime and a reboot row that asks for confirmation. Models with 4 rows leave out the last of these to keep the reboot row. To also show the panel's temperature, give it a sensor:

```yaml
nspanel_lovelace:
  temperature_sensor: panel_temperature
```

The services are `lock_panel`, `unlock_panel`, `set_lock_pin` (4 to 8 digits, empty removes the PIN and unlocks), `set_lock_pin_hash` (what the layout editor uses to give every panel one PIN), `set_button_lock`, `set_settings_access` (`open`, `pin` or `hidden`) and `set_lock_display` (`keypad` or `screensaver`). `pin` asks for the PIN each time settings is opened. While locked, `keypad` always shows the keypad and the screen stays at its normal brightness. `screensaver` shows the screensaver, a tap brings up the keypad and it goes back after the timeout. Everything survives a reboot. The panel stores a hash of the PIN and waits 30 seconds after 5 wrong attempts. This keeps people from using the screen, it doesn't protect against someone with access to the device or to Home Assistant.

`press_button` (see [Buttons and relays](#buttons-and-relays)) already respects **Lock buttons**. If your buttons do something else, check the state yourself:

```yaml
on_press:
  - if:
      condition:
        lambda: return !id(nspanel).buttons_locked();
      then:
        - light.toggle: light1
```

## Brightness rules

A runtime layout can set the brightness by time or from a Home Assistant entity, with a `brightness` key in the layout. The rule runs on the panel, and the slider and screen light still work until the rule's next change. `set_display_inactive_dim` and `set_display_active_dim` are ignored while a rule is set.

```json
{"brightness": {"mode": "schedule", "schedule": [
  {"time": "09:00", "active": 100, "inactive": 30},
  {"time": "21:00", "active": 60, "inactive": 1}
]}}
```

A schedule has up to 8 times and needs a `time_id` in the device configuration. `active` is 5 to 100 and `inactive`, the screensaver brightness, is 0 to 100.

```json
{"brightness": {"mode": "entity", "entity_id": "sensor.illuminance", "from": 0, "to": 200, "inactive_low": 1, "inactive_high": 30}}
```

`from` and `to` are the entity values that give the lowest and highest brightness. `inactive_low` and `inactive_high` are the screensaver brightness at those two values. They are optional, and setting both to the same value keeps it fixed.

## Buttons and relays

List the relays in the device configuration, and a runtime layout can then decide what each physical button does. This needs `runtime_layout: true`.

```yaml
nspanel_lovelace:
  relays:
    - light: light1
      switch: switch1
      button: button1
    - light: light2
      switch: switch2
      button: button2

binary_sensor:
  - platform: gpio
    id: button1
    on_press:
      - lambda: id(nspanel).press_button(0);
```

```json
{"buttons": {"left": {"action": "entity", "entity_id": "light.ceiling"}, "right": {"action": "none"}}}
```

`action` is `left` or `right` to toggle that relay, `entity` to command `entity_id`, or `none`. Without one, a button toggles its own relay. `entity` needs Home Assistant, so the button does nothing while it is offline.

The panel reports the names of these entities in its status event. Keep them out of `internal` and set `disabled_by_default: true` on the ones you don't normally want, and the layout editor enables or disables them in Home Assistant. This works the same on every ESPHome version, and a light and a switch keep their own entity IDs when you change between them.

## Screen light

Add lights that control the screen. The brightness of `screen_light` is the normal brightness, and turning it off turns the screen off until it's tapped or turned back on. The brightness of `screensaver_light` is the brightness on the screensaver, and turning it off makes that 0. The screensaver brightness is kept below the normal one. These need the panel settings, which are on unless you turn them off.

```yaml
nspanel_lovelace:
  screen_light:
    name: Screen
  screensaver_light:
    name: Screensaver
```

# Help Needed!

As much as I love embedded programming, I am unable to dedicate the time required to make this project great without the help of others.
This is a **beta project** which is not fully functional yet and is subject to change without notice. It has taken a lot of time and effort to get the project to this point and it will take a lot more time to get the project past beta.

There are many UI components missing and the [python build script](components/nspanel_lovelace/__init__.py) is in dire need of refactoring (I butchered it together to get it working but PRs to the ESPHome repo are required to provide more flexibility over code generation).

Currently the following features work:
- Screensaver with time, date, weather and status icon display
- Support for `cardGrid`, `cardGrid2`, `cardEntities`, `cardQR`, `cardAlarm`, `cardThermo`, `cardMedia`
- Most entity types should display on cards. Lights, switches, sensors and scenes have been tested to work, with additional support for the `popupLight` and `popupTimer` pages.

There is currently no support for these cards: `cardPower`. `cardUnlock`, `cardChart` - but these are planned for the future.
Please see the [HMI readme](https://github.com/joBr99/nspanel-lovelace-ui/tree/main/HMI) for more info on the cards mentioned above.

PRs to expand the functionality or fix bugs are very welcome!

# Known Issues

### 1. Weather forecast is not displayed on the screensaver when using Home Assistant 2024.4 or later

This issue is due to the `forecast` attribute being removed from weather entities. There is currently no alternative way to fetch this data with the current ESPHome functionality but I hope to get this fixed (see [this feature request](https://github.com/esphome/feature-requests/issues/2703)).
More info on the issue can be [found here](https://github.com/olicooper/esphome-nspanel-lovelace-native/issues/8).

**Solution**: Use the `service` forecast method to manually push weather data to the panel.
1. Update your ESPHome configuration:
    ```yaml
      screensaver:
        weather:
          entity_id: weather.home # Change this to your weather entity
          forecast_method: service
    ```
2. Add an automation to Home Assistant to push the forecast:
    ```yaml
    alias: NSPanel Lovelace Weather Forecast Sync
    description: >-
      This automation fetches the weather forecast and pushes only the upcoming
      entries to the NSPanel. It filters the forecast to only include entries
      starting from the current hour, keeping only datetime, condition, and
      temperature as these are the only attributes used by the nspanel.
    triggers:
      - minutes: /30
        trigger: time_pattern
      - entity_id: weather.home # Change this to your weather entity
        trigger: state
      ## NOTE: This makes sure the forecast is sent when the panel is turned on
      - trigger: state
        entity_id:
          - switch.nspanel_screen_power # Change this to your NSPanel 'screen_power' entity_id
        for:
          hours: 0
          minutes: 0
          seconds: 2
        to:
          - "on"
        alias: When nspanel becomes available
    conditions: []
    actions:
      - target:
          entity_id: weather.home # Change this to your weather entity
        data:
          type: hourly # or daily/twice_daily
        response_variable: forecast_data
        action: weather.get_forecasts
      - variables:
          forecast: >-
            {% set forecast = (forecast_data.values() | first).forecast %}
            {% set ns = namespace(items=[]) %}
            {% for item in forecast if as_timestamp(item.datetime) >= as_timestamp(now()) %}
              {% if (ns.items | length) < 5 %}
                {% set ns.items = ns.items + [{
                  "datetime": item.datetime | as_datetime | as_timestamp | timestamp_custom('%Y-%m-%dT%H:%M:%S'),
                  "condition": item.condition,
                  "temperature": item.temperature | float(1)
                }] %}
              {% endif %}
            {% endfor %}
            {{- ns.items }}
      - action: esphome.nspanel_set_weather_forecast_data # Change this to your NSPanel 'set_weather_forecast_data' action/service name. This can be found in HA developer tools > actions (after the nspanel has been re-configured to use the 'service' forecast_method and has connected to HA).
        data:
          forecast: "{{ forecast }}"
    mode: single
    ```

**Alternative Workaround (deprecated)**: Add a template sensor to your Home Assistant configuration (changing `weather.home` to your actual weather entity_id) then update the `weather` `entity_id` in your esphome config to the `unique_id` seen below (i.e. `sensor.weather_forecast_daily`).

```yaml
template:
  - trigger:
      - platform: time_pattern
        minutes: /30
      - platform: homeassistant
        event: start
      - platform: event
        event_type: event_template_reloaded
    action:
      - service: weather.get_forecasts
        data:
          type: daily
        target:
          entity_id: weather.home # Change this to your weather entity
        response_variable: data
    sensor:
      - name: Weather Forecast Daily
        unique_id: weather_forecast_daily # Use this id in your esphome config (screensaver -> weather -> entity_id)
        state: "{{ states('weather.home') }}" # Change this to your weather entity
        attributes:
          temperature: "{{ state_attr('weather.home', 'temperature') }}" # Change this to your weather entity
          temperature_unit: "{{ state_attr('weather.home', 'temperature_unit') }}" # Change this to your weather entity
          # Change 'weather.home' below to your weather entity
          forecast: >
            {%- with ns = namespace(arr=[],obj={}) -%}
            {%- for n in data['weather.home'].forecast[:5] -%}
              {%- for k, v in n | items -%}
                {%- if k == "datetime" -%}
                  {%- set ns.obj = dict(ns.obj | items, **{k: as_datetime(v, "0" | as_datetime) | as_local | as_timestamp | timestamp_custom('%Y-%m-%dT%H:%M:%S')}) -%}
                {%- elif k == "condition" or k == "temperature" -%}
                  {%- set ns.obj = dict(ns.obj | items, **{k: v}) -%}
                {%- endif -%}
              {%- endfor -%}
              {%- if (ns.obj.items() | length) == 3 and ns.obj.datetime -%}
                {%- set ns.arr = ns.arr + [ns.obj] -%}
              {%- endif -%}
            {%- endfor %}{{ns.arr}}{% endwith -%}
```

# License

Code in this repository is licensed under the GPLv3 license. Third-party code used in this project have their own license terms. Please see [the license document](LICENSE) for more information.
