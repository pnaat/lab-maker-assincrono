import time
import board
import adafruit_dht
import adafruit_bmp280
from gpiozero import LED, Button

DHT11Sensor = adafruit_dht.DHT11(board.D16)

ledRed = LED(13)
button = Button(20)

def collect_data():
    try:
        temperature_dht = DHT11Sensor.temperature
        humidity = DHT11Sensor.humidity
        button_pressed = button.is_pressed
        return temperature_dht, humidity, button_pressed
    except RuntimeError:
        return None, None, None

def led_status():
    ledRedSts = ledRed.is_lit
    return ledRedSts


def control_leds(red):
    ledRed.on() if red else ledRed.off()


if __name__ == "__main__":
    while True:
        ledRedSts  = led_status()
        temp_dht, hum, button_state  = collect_data()

        #control_leds(True, True, True)
         
        if all(v is not None for v in [temp_dht, hum, press]):
            print(f"\nMonitor Data")
            print(f"DHT11 Temp: {temp_dht:.1f}°C, Humidity: {hum:.1f}%")
            print(f"Button {'pressed' if button_state else 'not pressed'}")
            print(f"Red LED {'is on' if ledRedSts else 'is off'}")

            

        time.sleep(2)
