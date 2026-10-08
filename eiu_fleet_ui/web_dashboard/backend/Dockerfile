# Web backend image: plain Python, no ROS. Open-RMF is reached through eiu_rmf_gateway over Redis.
# --build-arg WITH_TESTS=true adds the test libraries (requirements-dev.txt).
FROM python:3.12-slim

ARG WITH_TESTS=false
COPY requirements.txt requirements-dev.txt /tmp/eiu_web/
RUN if [ "$WITH_TESTS" = "true" ]; then file=requirements-dev.txt; else file=requirements.txt; fi \
 && pip install --no-cache-dir -r /tmp/eiu_web/$file && rm -rf /tmp/eiu_web

WORKDIR /eiu_fleet_ui/web_dashboard/backend
ENTRYPOINT ["python3", "-m", "eiu_web_backend"]
CMD ["serve"]
