# Living Memory ~ a gift to my gf <3

## Why?

I wanted a way to connect to my gf without relying on whatsapp or instagram and all of the ready to buy screens either came with an app or was missing one of the features I imagined. So I decided to buy a small e-ink display, the LilyGo Screen, which comes with the ESP32S3, and the freedom to try and put anything on it.

## What can it do?

I then decided to create 4 Partitions:

- calendar
- weather
- days to meet
- daily memory

This way she could potentially have something useful, such as seeing the current weather for today and the following days and also the current date, time and month calendar view.

Apart from the useful generic stuff, the two sections on the right are what makes this personal. The top panel shows the remaining days until we see each other again in the form of a letter, which, when tapped, shows a daily note that I can edit from a google sheet. Then we have the bottom panel, which draws a polaroid written 'daily memory' and upon tapped, shows one of the many pictures I saved to the external microsd card. They both update daily :)

## Why google sheets

Google Sheets allowed me to easily format all my messages and easily synchronize them with with app scripts -> [[sheets.gs]]. It was also mandatory to have somehow access to the data while away from the device, since I wouldn't be able to recompile and flash it.

## Overall

This was also a chance a had to learn more about embedded software and how to deal with some of the hardware aspects of it. For a next embedded project I wish to do something which also involves soldering, other external, more complex components, making the hardware side more interesting.
