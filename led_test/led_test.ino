void setup() {
  pinMode(19, OUTPUT);
  Serial.begin(115200);
  Serial.println("LED 제어 시작!");
}

void loop() {
  digitalWrite(19, HIGH);
  Serial.println("LED ON");
  delay(3000);
  
  digitalWrite(19, LOW);
  Serial.println("LED OFF");
  delay(1000);
}