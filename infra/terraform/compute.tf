# One instance (Amazon Linux 2023, Docker + Compose from the user data), an Elastic IP, and a small separate data volume for /var/lib/napple.
# Cost choices: no NAT gateway, no load balancer, no detailed monitoring, on-demand (no Spot: stability), gp3 at the minimum sizes.
# The containers are not started here: every deploy (infra/scripts/roll.sh, through SSM send-command) writes the compose files and .env and runs
# `docker compose pull && up -d`, so a replaced instance comes back on the next workflow run.
data "aws_ssm_parameter" "al2023" {
  name = "/aws/service/ami-amazon-linux-latest/al2023-ami-kernel-default-${var.architecture}"
}

resource "aws_ebs_volume" "data" {
  count             = var.data_volume_gb > 0 ? 1 : 0
  availability_zone = local.az
  size              = var.data_volume_gb
  type              = "gp3"
  encrypted         = true
  tags              = { Name = "${local.name}-data" }
}

resource "aws_instance" "servers" {
  ami                    = nonsensitive(data.aws_ssm_parameter.al2023.value)
  instance_type          = var.instance_type
  subnet_id              = aws_subnet.public.id
  vpc_security_group_ids = [aws_security_group.servers.id]
  iam_instance_profile   = aws_iam_instance_profile.instance.name
  monitoring             = false

  user_data = templatefile("${path.module}/user_data.sh.tftpl", {
    compose_version = var.compose_version
    data_volume_id  = var.data_volume_gb > 0 ? replace(aws_ebs_volume.data[0].id, "-", "") : ""
  })

  metadata_options {
    http_tokens = "required"
    # 2: the containers (napple-account: SES v2 mail + S3 photos/saves with the instance role) reach IMDSv2 through the Docker bridge, one hop
    # more than the host; with 1 the token reply is dropped and the account server has no AWS credentials.
    http_put_response_hop_limit = 2
    http_endpoint               = "enabled"
  }

  root_block_device {
    volume_size = var.root_volume_gb
    volume_type = "gp3"
    encrypted   = true
  }

  # standard: a burstable instance that runs out of CPU credits slows down instead of billing surplus credits.
  credit_specification {
    cpu_credits = "standard"
  }

  tags = { Name = local.name }

  lifecycle {
    # A newer AL2023 AMI must not replace the running server on an unrelated apply; replace on purpose with `-replace=aws_instance.servers`.
    ignore_changes = [ami]
  }
}

resource "aws_volume_attachment" "data" {
  count       = var.data_volume_gb > 0 ? 1 : 0
  device_name = "/dev/sdf"
  volume_id   = aws_ebs_volume.data[0].id
  instance_id = aws_instance.servers.id
}

resource "aws_eip" "servers" {
  domain = "vpc"
  tags   = { Name = local.name }
}

resource "aws_eip_association" "servers" {
  allocation_id = aws_eip.servers.id
  instance_id   = aws_instance.servers.id
}
