#!/usr/bin/env bash

# http://stackoverflow.com/a/26236508/4603498

file=$1
bucket="cdn.rsa.pub"
resource="/${bucket}/${file}"
contentType="application/x-itunes-ipa"
dateValue=`date -R`
stringToSign="PUT\n\n${contentType}\n${dateValue}\n${resource}"
s3Key=$AWS_ACCESS_KEY_ID
s3Secret=$AWS_SECRET_ACCESS_KEY

signature=`echo -en ${stringToSign} | openssl sha1 -hmac ${s3Secret} -binary | base64`
curl -X PUT -k -s -w "%{http_code}\\n" -T "${file}" \
 -H "Host: ${bucket}.s3.amazonaws.com" \
 -H "Date: ${dateValue}" \
 -H "Content-Type: ${contentType}" \
 -H "Authorization: AWS ${s3Key}:${signature}" \
 https://${bucket}.s3.amazonaws.com/${file}

# should return 200 on success