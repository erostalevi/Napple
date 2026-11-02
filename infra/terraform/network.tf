# A VPC of its own with one public subnet (no NAT gateway, no cost): the instance reaches ECR, SSM and CloudWatch through the internet gateway.
locals {
  name = "napple-servers"
  az   = "${var.region}${var.az_suffix}"
}

resource "aws_vpc" "main" {
  cidr_block           = var.vpc_cidr
  enable_dns_support   = true
  enable_dns_hostnames = true
  tags                 = { Name = local.name }
}

resource "aws_internet_gateway" "main" {
  vpc_id = aws_vpc.main.id
  tags   = { Name = local.name }
}

resource "aws_subnet" "public" {
  vpc_id            = aws_vpc.main.id
  cidr_block        = cidrsubnet(var.vpc_cidr, 8, 1)
  availability_zone = local.az
  tags              = { Name = "${local.name}-public" }
}

resource "aws_route_table" "public" {
  vpc_id = aws_vpc.main.id
  route {
    cidr_block = "0.0.0.0/0"
    gateway_id = aws_internet_gateway.main.id
  }
  tags = { Name = "${local.name}-public" }
}

resource "aws_route_table_association" "public" {
  subnet_id      = aws_subnet.public.id
  route_table_id = aws_route_table.public.id
}

# Inbound: HTTP(S) for Caddy (80 is needed for the ACME HTTP-01 challenge and the HTTPS redirect), the NEX UDP range and the room port.
# No SSH: shell access is SSM Session Manager (`aws ssm start-session --target <instance id>`).
resource "aws_security_group" "servers" {
  name        = local.name
  description = "Napple game servers: HTTPS API, NEX/PRUDP UDP, room relay"
  vpc_id      = aws_vpc.main.id
  tags        = { Name = local.name }
}

locals {
  ingress = {
    http     = { proto = "tcp", from = 80, to = 80 }
    https    = { proto = "tcp", from = 443, to = 443 }
    https_h3 = { proto = "udp", from = 443, to = 443 }
    nex      = { proto = "udp", from = var.nex_udp_from, to = var.nex_udp_to }
    room_tcp = { proto = "tcp", from = var.room_port, to = var.room_port }
    room_udp = { proto = "udp", from = var.room_port, to = var.room_port }
  }
}

resource "aws_vpc_security_group_ingress_rule" "v4" {
  for_each          = local.ingress
  security_group_id = aws_security_group.servers.id
  description       = each.key
  ip_protocol       = each.value.proto
  from_port         = each.value.from
  to_port           = each.value.to
  cidr_ipv4         = "0.0.0.0/0"
}

resource "aws_vpc_security_group_egress_rule" "all" {
  security_group_id = aws_security_group.servers.id
  description       = "outbound (ECR, SSM, CloudWatch, ACME, OS updates)"
  ip_protocol       = "-1"
  cidr_ipv4         = "0.0.0.0/0"
}
